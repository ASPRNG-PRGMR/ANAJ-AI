/* Edge Impulse Arduino examples
 * Copyright (c) 2022 EdgeImpulse Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// These sketches are tested with 2.0.4 ESP32 Arduino Core
// https://github.com/espressif/arduino-esp32/releases/tag/2.0.4


/* Includes ---------------------------------------------------------------- */
#include <arduino-hackathon-project_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"

#include "esp_camera.h"

// Select camera model - find more camera models in camera_pins.h file here
// https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/Camera/CameraWebServer/camera_pins.h

#define CAMERA_MODEL_ESP32S3_EYE
//#define CAMERA_MODEL_ESP_EYE // Has PSRAM
//#define CAMERA_MODEL_AI_THINKER // Has PSRAM

#if defined(CAMERA_MODEL_ESP_EYE)
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    4
#define SIOD_GPIO_NUM    18
#define SIOC_GPIO_NUM    23

#define Y9_GPIO_NUM      36
#define Y8_GPIO_NUM      37
#define Y7_GPIO_NUM      38
#define Y6_GPIO_NUM      39
#define Y5_GPIO_NUM      35
#define Y4_GPIO_NUM      14
#define Y3_GPIO_NUM      13
#define Y2_GPIO_NUM      34
#define VSYNC_GPIO_NUM   5
#define HREF_GPIO_NUM    27
#define PCLK_GPIO_NUM    25

#elif defined(CAMERA_MODEL_ESP32S3_EYE)
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5

#define Y2_GPIO_NUM 11
#define Y3_GPIO_NUM 9
#define Y4_GPIO_NUM 8
#define Y5_GPIO_NUM 10
#define Y6_GPIO_NUM 12
#define Y7_GPIO_NUM 18
#define Y8_GPIO_NUM 17
#define Y9_GPIO_NUM 16

#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

#elif defined(CAMERA_MODEL_AI_THINKER)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#else
#error "Camera model not selected"
#endif

/* Constant defines -------------------------------------------------------- */
#define EI_CAMERA_RAW_FRAME_BUFFER_COLS           320
#define EI_CAMERA_RAW_FRAME_BUFFER_ROWS           240
#define EI_CAMERA_FRAME_BYTE_SIZE                 3

/* Private variables ------------------------------------------------------- */
static bool debug_nn = false; // Set this to true to see e.g. features generated from the raw signal
static bool is_initialised = false;
uint8_t *snapshot_buf; //points to the output of the capture

static camera_config_t camera_config = {
    .pin_pwdn = PWDN_GPIO_NUM,
    .pin_reset = RESET_GPIO_NUM,
    .pin_xclk = XCLK_GPIO_NUM,
    .pin_sscb_sda = SIOD_GPIO_NUM,
    .pin_sscb_scl = SIOC_GPIO_NUM,

    .pin_d7 = Y9_GPIO_NUM,
    .pin_d6 = Y8_GPIO_NUM,
    .pin_d5 = Y7_GPIO_NUM,
    .pin_d4 = Y6_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM,
    .pin_d2 = Y4_GPIO_NUM,
    .pin_d1 = Y3_GPIO_NUM,
    .pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM,
    .pin_href = HREF_GPIO_NUM,
    .pin_pclk = PCLK_GPIO_NUM,

    //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG, //YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_QVGA,    //QQVGA-UXGA Do not use sizes above QVGA when not JPEG

    .jpeg_quality = 12, //0-63 lower number means higher quality
    .fb_count = 1,       //if more than one, i2s runs in continuous mode. Use only with JPEG
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

/* Phase 4 -- TinyML feature extractor (runs on the SAME frame the CNN just
 * classified) --------------------------------------------------------------
 * IMPORTANT: ei_camera_capture() resizes snapshot_buf IN PLACE down to
 * EI_CLASSIFIER_INPUT_WIDTH x EI_CLASSIFIER_INPUT_HEIGHT (the model's input
 * resolution), not the raw 320x240 capture size. So by the time we get here
 * (after run_classifier()), snapshot_buf holds a smaller image than the
 * EI_CAMERA_RAW_FRAME_BUFFER_* constants suggest. extractFeatures() below
 * is called with EI_CLASSIFIER_INPUT_WIDTH/HEIGHT for exactly this reason --
 * using the raw capture dims here would read garbage past the actual image
 * data.
 *
 * KEEP GRID_SIZE IN SYNC WITH training/train_tiny_model.py -- if you change
 * one, change the other and retrain.
 *
 * ****** FEATURE SET v2 -- color + texture, not just color ******
 * v1 used [meanR, meanG, meanB, greenRatio] per cell -- four flavors of
 * "what color is this patch," which made the Uno's model a weaker echo of
 * the CNN rather than a meaningfully independent check. v2 replaces that
 * with two color cues and two texture cues per cell:
 *   [meanBrightness, greenRatio, edgeDensity, contrast]
 * - meanBrightness: overall light/dark of the cell (grayscale mean)
 * - greenRatio: kept from v1 -- real disease-relevant color cue
 * - edgeDensity: mean pixel-to-pixel brightness jump, scanning
 *   horizontally across the cell. Smooth healthy tissue -> low. Blotchy/
 *   lesioned tissue -> high. This is texture, not color.
 * - contrast: std-dev of brightness within the cell. Catches "busy"
 *   patches (spotting, lesion boundaries) even when the average color
 *   looks unremarkable.
 * IF YOU CHANGE THIS: mirror it exactly in extract_features() in
 * training/train_tiny_model.py, and retrain -- a stale tiny_model.h
 * trained on v1's feature meaning will produce garbage predictions on
 * v2 feature values even though the array is still 64 uint8_t values
 * and compiles fine. The shape staying the same is exactly what makes
 * this mistake easy to make silently -- there's no compile-time check
 * that catches "trained on the wrong feature semantics."
 */
#define GRID_SIZE       4
#define STATS_PER_CELL  4                                          // meanBrightness, greenRatio, edgeDensity, contrast
#define FEATURE_COUNT   (GRID_SIZE * GRID_SIZE * STATS_PER_CELL)   // 64

// Fast integer square root (no float, no <math.h> dependency for this).
// Standard bit-shift method -- exact for uint32_t input.
uint16_t isqrt(uint32_t n) {
  uint32_t res = 0;
  uint32_t bit = 1UL << 30;
  while (bit > n) bit >>= 2;
  while (bit != 0) {
    if (n >= res + bit) {
      n -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return (uint16_t)res;
}

void extractFeatures(const uint8_t *rgb888, int width, int height, uint8_t *features) {
  int cellW = width / GRID_SIZE;
  int cellH = height / GRID_SIZE;
  int outIdx = 0;

  for (int gy = 0; gy < GRID_SIZE; gy++) {
    for (int gx = 0; gx < GRID_SIZE; gx++) {
      long sumR = 0, sumG = 0, sumB = 0;
      long sumGray = 0;
      long sumGraySq = 0;
      long sumHEdge = 0;
      long count = 0;

      int startX = gx * cellW;
      int startY = gy * cellH;
      int endX = (gx == GRID_SIZE - 1) ? width : startX + cellW;
      int endY = (gy == GRID_SIZE - 1) ? height : startY + cellH;

      for (int y = startY; y < endY; y++) {
        const uint8_t *rowPtr = &rgb888[(y * width + startX) * 3];
        int prevGray = -1; // sentinel: no previous pixel yet this row

        for (int x = startX; x < endX; x++) {
          uint8_t r = rowPtr[0], g = rowPtr[1], b = rowPtr[2];
          sumR += r; sumG += g; sumB += b;

          int gray = (r + g + b) / 3;
          sumGray += gray;
          sumGraySq += (long)gray * gray;

          if (prevGray >= 0) {
            int diff = gray - prevGray;
            if (diff < 0) diff = -diff;
            sumHEdge += diff;
          }
          prevGray = gray;

          rowPtr += 3;
          count++;
        }
      }

      uint8_t meanBrightness = (count > 0) ? (uint8_t)(sumGray / count) : 0;

      long total = sumR + sumG + sumB;
      uint8_t greenRatio = (total > 0) ? (uint8_t)((sumG * 255L) / total) : 0;

      // Edge density: mean abs horizontal brightness jump. Denominator
      // uses `count` (not count-rows) as a cheap approximation -- close
      // enough for a coarse texture signal, not worth the extra bookkeeping.
      uint8_t edgeDensity = (count > 0) ? (uint8_t)min(255L, sumHEdge / count) : 0;

      // Contrast: std-dev of brightness within the cell, via integer sqrt
      // of population variance. Max possible value is well within uint8_t
      // range (worst case ~127 for a fully bimodal 0/255 split), so no
      // clamping needed here.
      uint8_t contrast = 0;
      if (count > 0) {
        long meanGrayL = sumGray / count;
        long variance = (sumGraySq / count) - (meanGrayL * meanGrayL);
        if (variance < 0) variance = 0; // integer rounding can nudge this slightly negative
        contrast = (uint8_t)isqrt((uint32_t)variance);
      }

      features[outIdx++] = meanBrightness;
      features[outIdx++] = greenRatio;
      features[outIdx++] = edgeDensity;
      features[outIdx++] = contrast;
    }
  }
}

void printFeatures(const uint8_t *features) {
  Serial.println("Features:");
  Serial.print("  ");
  for (int i = 0; i < FEATURE_COUNT; i++) {
    Serial.print(features[i]);
    if (i < FEATURE_COUNT - 1) Serial.print(",");
  }
  Serial.println();

  // Ready-to-paste line for the Uno's serial test harness (uno_tinyml.ino).
  Serial.print("[COPY TO UNO] TEST,");
  for (int i = 0; i < FEATURE_COUNT; i++) {
    Serial.print(features[i]);
    if (i < FEATURE_COUNT - 1) Serial.print(",");
  }
  Serial.println();
}

/* Function definitions ------------------------------------------------------- */
bool ei_camera_init(void);
void ei_camera_deinit(void);
bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) ;

/* Phase 3 -- UART link to the Uno R3 -----------------------------------
 * GPIO 38/39 -- chosen specifically to avoid conflicts on this board:
 *   - GPIO 43/44 are UART0, which IS your active Serial/Serial Monitor
 *     connection on this board (Upload Mode: "UART0 / Hardware CDC" with
 *     USB CDC On Boot disabled) -- using them for a second UART here
 *     would fight with and break the console you're reading output on.
 *   - GPIO 26-37 are reserved for this module's octal flash + octal PSRAM
 *     (N16R8) -- never safe to repurpose as GPIO.
 *   - GPIO 19/20 are native USB D-/D+.
 * 38/39 are free general-purpose pins outside all of that. Wire the Uno's
 * D2/D3 to these physical pins instead of 44/43.
 */
#define UART_TO_UNO_RX_PIN  38
#define UART_TO_UNO_TX_PIN  39
#define UART_TO_UNO_BAUD    9600
HardwareSerial UnoLink(1);

/**
* @brief      Arduino setup function
*/
void setup()
{
    // put your setup code here, to run once:
    Serial.begin(115200);
    //comment out the below line to start inference immediately after upload
    while (!Serial);
    Serial.println("Edge Impulse Inferencing Demo");
    if (ei_camera_init() == false) {
        ei_printf("Failed to initialize Camera!\r\n");
    }
    else {
        ei_printf("Camera initialized\r\n");
    }

    // Phase 4: this is the resolution extractFeatures() will actually see
    // (snapshot_buf gets resized to this in place during capture -- see the
    // comment above extractFeatures()). Use these exact numbers for
    // --resize-w / --resize-h when running train_tiny_model.py, so the
    // Python-side feature extraction matches this board's, pixel for pixel.
    ei_printf("Classifier input resolution: %dx%d (use this for train_tiny_model.py --resize-w/--resize-h)\n",
              EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT);

    // Phase 3: UART link to the Uno.
    UnoLink.begin(UART_TO_UNO_BAUD, SERIAL_8N1, UART_TO_UNO_RX_PIN, UART_TO_UNO_TX_PIN);
    ei_printf("UART to Uno up on RX=%d TX=%d @ %d baud.\n",
              UART_TO_UNO_RX_PIN, UART_TO_UNO_TX_PIN, UART_TO_UNO_BAUD);

    ei_printf("\nWaiting for SCAN from Uno (or type SCAN into this Serial Monitor to test locally)...\n");
}

/* Phase 3 -- UART link to the Uno R3 -----------------------------------
 * Same pins as the earlier standalone Phase 3 sketch: this board's RX/TX
 * on GPIO 44/43 (outside the camera pin block and native USB), talking to
 * the Uno's SoftwareSerial on D2/D3. Baud must match the Uno side (9600).
 */

// Finds the highest-scoring class from the CNN result -- this becomes the
// "class"/"confidence" fields sent to the Uno.
void getBestClass(const ei_impulse_result_t &result, const char **outLabel, float *outConfidence) {
  int bestIdx = 0;
  float bestVal = result.classification[0].value;
  for (uint16_t i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > bestVal) {
      bestVal = result.classification[i].value;
      bestIdx = i;
    }
  }
  *outLabel = ei_classifier_inferencing_categories[bestIdx];
  *outConfidence = bestVal;
}

// Sends the §6-style JSON result line to the Uno:
// {"class":"...","confidence":0.93,"feat":[64 comma-separated ints]}\n
void sendResultToUno(const char *className, float confidence, const uint8_t *features) {
  static char line[420]; // class name + confidence + 64 features comfortably fits with room to spare
  int offset = 0;

  offset += snprintf(line + offset, sizeof(line) - offset,
                      "{\"class\":\"%s\",\"confidence\":%.2f,\"feat\":[",
                      className, confidence);
  for (int i = 0; i < FEATURE_COUNT; i++) {
    offset += snprintf(line + offset, sizeof(line) - offset,
                        "%d%s", features[i], (i < FEATURE_COUNT - 1) ? "," : "");
  }
  offset += snprintf(line + offset, sizeof(line) - offset, "]}\n");

  UnoLink.print(line);
  Serial.print("[TX -> Uno] ");
  Serial.print(line);
}

// Reads one '\n'-terminated line from the Uno link, non-blocking.
// Returns "" if nothing complete has arrived yet.
String readLineFromUno() {
  static char buf[16];
  static size_t len = 0;

  while (UnoLink.available()) {
    char c = UnoLink.read();
    if (c == '\n') {
      buf[len] = '\0';
      String line = String(buf);
      len = 0;
      return line;
    }
    if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      len = 0; // overflow guard -- "SCAN" is short, this shouldn't trigger
    }
  }
  return String();
}

// The full capture -> classify -> extract -> send sequence. This is your
// original loop() body, refactored into a function so it can be triggered
// by an incoming SCAN command instead of running continuously -- matching
// the Phase 3 protocol (Uno asks, ESP32-S3-CAM answers) instead of the
// free-running loop the stock EI example uses.
void performScanAndRespond() {
    snapshot_buf = (uint8_t*)malloc(EI_CAMERA_RAW_FRAME_BUFFER_COLS * EI_CAMERA_RAW_FRAME_BUFFER_ROWS * EI_CAMERA_FRAME_BYTE_SIZE);

    if(snapshot_buf == nullptr) {
        ei_printf("ERR: Failed to allocate snapshot buffer!\n");
        return;
    }

    ei::signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data = &ei_camera_get_data;

    if (ei_camera_capture((size_t)EI_CLASSIFIER_INPUT_WIDTH, (size_t)EI_CLASSIFIER_INPUT_HEIGHT, snapshot_buf) == false) {
        ei_printf("Failed to capture image\r\n");
        free(snapshot_buf);
        return;
    }

    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);
    if (err != EI_IMPULSE_OK) {
        ei_printf("ERR: Failed to run classifier (%d)\n", err);
        free(snapshot_buf);
        return;
    }

    ei_printf("Predictions (DSP: %d ms., Classification: %d ms., Anomaly: %d ms.): \n",
                result.timing.dsp, result.timing.classification, result.timing.anomaly);

#if EI_CLASSIFIER_OBJECT_DETECTION == 1
    ei_printf("Object detection bounding boxes:\r\n");
    for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
        if (bb.value == 0) {
            continue;
        }
        ei_printf("  %s (%f) [ x: %u, y: %u, width: %u, height: %u ]\r\n",
                bb.label,
                bb.value,
                bb.x,
                bb.y,
                bb.width,
                bb.height);
    }
#else
    ei_printf("Predictions:\r\n");
    for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        ei_printf("  %s: ", ei_classifier_inferencing_categories[i]);
        ei_printf("%.5f\r\n", result.classification[i].value);
    }
#endif

#if EI_CLASSIFIER_HAS_ANOMALY
    ei_printf("Anomaly prediction: %.3f\r\n", result.anomaly);
#endif

#if EI_CLASSIFIER_HAS_VISUAL_ANOMALY
    ei_printf("Visual anomalies:\r\n");
    for (uint32_t i = 0; i < result.visual_ad_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.visual_ad_grid_cells[i];
        if (bb.value == 0) {
            continue;
        }
        ei_printf("  %s (%f) [ x: %u, y: %u, width: %u, height: %u ]\r\n",
                bb.label,
                bb.value,
                bb.x,
                bb.y,
                bb.width,
                bb.height);
    }
#endif

    static uint8_t features[FEATURE_COUNT];
    extractFeatures(snapshot_buf, EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT, features);
    printFeatures(features);

    free(snapshot_buf);

    // Phase 3: send the real CNN class + confidence + real feat[] to the Uno.
#if EI_CLASSIFIER_OBJECT_DETECTION != 1
    const char *bestLabel;
    float bestConfidence;
    getBestClass(result, &bestLabel, &bestConfidence);
    sendResultToUno(bestLabel, bestConfidence, features);
#endif
}

void loop()
{
    // From the Uno, over the real UART link.
    String line = readLineFromUno();
    if (line == "SCAN") {
        Serial.println("[RX <- Uno] SCAN");
        performScanAndRespond();
    }

    // Local test path via this board's own Serial Monitor -- type SCAN here
    // to trigger a scan without the Uno attached.
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toUpperCase();
        if (cmd == "SCAN") {
            Serial.println("[LOCAL TEST] Simulating SCAN trigger.");
            performScanAndRespond();
        }
    }
}

/**
 * @brief   Setup image sensor & start streaming
 *
 * @retval  false if initialisation failed
 */
bool ei_camera_init(void) {

    if (is_initialised) return true;

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

    //initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
      Serial.printf("Camera init failed with error 0x%x\n", err);
      return false;
    }

    sensor_t * s = esp_camera_sensor_get();
    // initial sensors are flipped vertically and colors are a bit saturated
    if (s->id.PID == OV3660_PID) {
      s->set_vflip(s, 1); // flip it back
      s->set_brightness(s, 1); // up the brightness just a bit
      s->set_saturation(s, 0); // lower the saturation
    }

#if defined(CAMERA_MODEL_M5STACK_WIDE)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
#elif defined(CAMERA_MODEL_ESP_EYE)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
    s->set_awb_gain(s, 1);
#endif

    is_initialised = true;
    return true;
}

/**
 * @brief      Stop streaming of sensor data
 */
void ei_camera_deinit(void) {

    //deinitialize the camera
    esp_err_t err = esp_camera_deinit();

    if (err != ESP_OK)
    {
        ei_printf("Camera deinit failed\n");
        return;
    }

    is_initialised = false;
    return;
}


/**
 * @brief      Capture, rescale and crop image
 *
 * @param[in]  img_width     width of output image
 * @param[in]  img_height    height of output image
 * @param[in]  out_buf       pointer to store output image, NULL may be used
 *                           if ei_camera_frame_buffer is to be used for capture and resize/cropping.
 *
 * @retval     false if not initialised, image captured, rescaled or cropped failed
 *
 */
bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) {
    bool do_resize = false;

    if (!is_initialised) {
        ei_printf("ERR: Camera is not initialized\r\n");
        return false;
    }

    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb) {
        ei_printf("Camera capture failed\n");
        return false;
    }

   bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf);

   esp_camera_fb_return(fb);

   if(!converted){
       ei_printf("Conversion failed\n");
       return false;
   }

    if ((img_width != EI_CAMERA_RAW_FRAME_BUFFER_COLS)
        || (img_height != EI_CAMERA_RAW_FRAME_BUFFER_ROWS)) {
        do_resize = true;
    }

    if (do_resize) {
        ei::image::processing::crop_and_interpolate_rgb888(
        out_buf,
        EI_CAMERA_RAW_FRAME_BUFFER_COLS,
        EI_CAMERA_RAW_FRAME_BUFFER_ROWS,
        out_buf,
        img_width,
        img_height);
    }


    return true;
}

static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr)
{
    // we already have a RGB888 buffer, so recalculate offset into pixel index
    size_t pixel_ix = offset * 3;
    size_t pixels_left = length;
    size_t out_ptr_ix = 0;

    while (pixels_left != 0) {
        // Swap BGR to RGB here
        // due to https://github.com/espressif/esp32-camera/issues/379
        out_ptr[out_ptr_ix] = (snapshot_buf[pixel_ix + 2] << 16) + (snapshot_buf[pixel_ix + 1] << 8) + snapshot_buf[pixel_ix];

        // go to the next pixel
        out_ptr_ix++;
        pixel_ix+=3;
        pixels_left--;
    }
    // and done!
    return 0;
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Invalid model for current sensor"
#endif
