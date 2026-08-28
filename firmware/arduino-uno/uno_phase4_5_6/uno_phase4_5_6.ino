/*
 * ================================================================
 * CROP SENTINEL AI - PHASE 6
 * Zone Tagging + History + TFT + Laptop Control
 * ================================================================
 *
 * Arduino UNO R3
 * SmartElex 2.8" ILI9341 TFT
 *
 * TFT:
 *   VCC  -> 5V
 *   GND  -> GND
 *   RESET -> D8
 *   DC    -> D9
 *   CS    -> D10
 *   MOSI  -> D11
 *   MISO  -> D12
 *   SCK   -> D13
 *   LED   -> 5V
 *
 * TOUCH:
 *   NOT USED
 *
 * Laptop:
 *   USB -> Arduino
 *   Serial Monitor -> 9600 baud
 *
 * ESP32:
 *   ESP32 TX -> Arduino D2
 *   ESP32 RX -> Arduino D3
 *   GND      -> GND
 *   ESP32 communication = 9600 baud
 *
 * Laptop commands:
 *
 *   SCAN:A1
 *   SCAN:B4
 *   SCAN:C3
 *   SCAN:E5
 *
 *   HOME
 *   ZONES
 *   HELP
 *
 * ================================================================
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SoftwareSerial.h>
#include "tiny_model.h"


// ================================================================
// TFT PINS
// ================================================================

#define TFT_CS      10
#define TFT_DC       9
#define TFT_RESET    8

// Hardware SPI:
// MOSI = D11
// MISO = D12
// SCK  = D13


// ================================================================
// ESP32 UART
// ================================================================

#define ESP_RX_PIN   2
#define ESP_TX_PIN   3
#define ESP_BAUD     9600


// ================================================================
// OBJECTS
// ================================================================

Adafruit_ILI9341 tft(
  TFT_CS,
  TFT_DC,
  TFT_RESET
);

SoftwareSerial espLink(
  ESP_RX_PIN,
  ESP_TX_PIN
);


// ================================================================
// SCREEN STATES
// ================================================================

enum AppScreen {

  SCREEN_IDLE,

  SCREEN_ZONE_PICK,

  SCREEN_SCANNING,

  SCREEN_RESULT
};

AppScreen currentScreen = SCREEN_IDLE;


// ================================================================
// CURRENT ZONE
// ================================================================

char currentZoneId[4] = "";


// ================================================================
// TINYML
// ================================================================

#define FEAT_LEN 64


#if FEATURE_COUNT != FEAT_LEN

#error "tiny_model.h FEATURE_COUNT must be 64"

#endif


// ================================================================
// ZONE HISTORY
// ================================================================

#define MAX_ZONES 3
#define SCANS_PER_ZONE 3
#define CLASS_NAME_LEN 12


struct ScanRecord {

  char className[CLASS_NAME_LEN];

  float confidence;

  unsigned long timestampMs;

  bool used;
};


struct ZoneSlot {

  char zoneId[4];

  bool inUse;

  uint8_t count;

  uint8_t nextWriteIndex;

  ScanRecord scans[SCANS_PER_ZONE];
};


ZoneSlot zoneHistory[MAX_ZONES];

uint8_t zoneCount = 0;


// ================================================================
// COLORS
// ================================================================

#define COLOR_BG        ILI9341_BLACK
#define COLOR_TEXT      ILI9341_WHITE
#define COLOR_HEALTHY   ILI9341_GREEN
#define COLOR_DISEASED  ILI9341_RED
#define COLOR_WARNING   ILI9341_YELLOW
#define COLOR_MUTED     ILI9341_LIGHTGREY
#define COLOR_BUTTON    ILI9341_DARKGREEN
#define COLOR_GRID      ILI9341_NAVY


// ================================================================
// TFT LAYOUT
// ================================================================

#define BTN_X 20
#define BTN_Y 250
#define BTN_W 200
#define BTN_H 50


#define GRID_COLS 5
#define GRID_ROWS 5

#define GRID_X 15
#define GRID_Y 60

#define GRID_CELL 40
#define GRID_GAP 6


// ================================================================
// FIND / CREATE ZONE
// ================================================================

ZoneSlot *findOrCreateZone(
  const char *zoneId
) {

  for (
    uint8_t i = 0;
    i < zoneCount;
    i++
  ) {

    if (
      strcmp(
        zoneHistory[i].zoneId,
        zoneId
      ) == 0
    ) {

      return &zoneHistory[i];
    }
  }


  if (
    zoneCount >= MAX_ZONES
  ) {

    return nullptr;
  }


  ZoneSlot &z =
    zoneHistory[zoneCount];


  strncpy(
    z.zoneId,
    zoneId,
    sizeof(z.zoneId) - 1
  );

  z.zoneId[
    sizeof(z.zoneId) - 1
  ] = '\0';


  z.inUse = true;

  z.count = 0;

  z.nextWriteIndex = 0;


  for (
    uint8_t i = 0;
    i < SCANS_PER_ZONE;
    i++
  ) {

    z.scans[i].used = false;
  }


  zoneCount++;


  return &z;
}


// ================================================================
// ADD HISTORY
// ================================================================

void addScanToHistory(
  const char *zoneId,
  const char *className,
  float confidence
) {

  ZoneSlot *z =
    findOrCreateZone(zoneId);


  if (!z) {

    Serial.println(
      F("[WARN] Maximum zone history reached.")
    );

    return;
  }


  ScanRecord &r =
    z->scans[z->nextWriteIndex];


  strncpy(
    r.className,
    className,
    CLASS_NAME_LEN - 1
  );

  r.className[
    CLASS_NAME_LEN - 1
  ] = '\0';


  r.confidence =
    confidence;


  r.timestampMs =
    millis();


  r.used = true;


  z->nextWriteIndex =
    (
      z->nextWriteIndex + 1
    ) % SCANS_PER_ZONE;


  if (
    z->count < SCANS_PER_ZONE
  ) {

    z->count++;
  }
}


// ================================================================
// PRINT HISTORY
// ================================================================

void printZoneHistory(
  const char *zoneId
) {

  for (
    uint8_t i = 0;
    i < zoneCount;
    i++
  ) {

    if (
      strcmp(
        zoneHistory[i].zoneId,
        zoneId
      ) != 0
    ) {

      continue;
    }


    ZoneSlot &z =
      zoneHistory[i];


    Serial.print(
      F("[HISTORY] Zone ")
    );

    Serial.print(zoneId);

    Serial.print(
      F(" - ")
    );

    Serial.print(z.count);

    Serial.println(
      F(" reading(s)")
    );


    for (
      uint8_t n = 0;
      n < z.count;
      n++
    ) {

      uint8_t idx =
        (
          z.nextWriteIndex
          + SCANS_PER_ZONE
          - z.count
          + n
        ) % SCANS_PER_ZONE;


      ScanRecord &r =
        z.scans[idx];


      if (!r.used) {
        continue;
      }


      Serial.print(
        F("  ")
      );

      Serial.print(
        r.className
      );

      Serial.print(
        F("  confidence=")
      );

      Serial.println(
        r.confidence,
        2
      );
    }


    return;
  }


  Serial.println(
    F("[HISTORY] No history for this zone.")
  );
}


// ================================================================
// GET PREVIOUS READING
// ================================================================

bool getPreviousReading(
  const char *zoneId,
  ScanRecord &out
) {

  for (
    uint8_t i = 0;
    i < zoneCount;
    i++
  ) {

    if (
      strcmp(
        zoneHistory[i].zoneId,
        zoneId
      ) != 0
    ) {

      continue;
    }


    ZoneSlot &z =
      zoneHistory[i];


    if (
      z.count < 2
    ) {

      return false;
    }


    uint8_t lastIndex =
      (
        z.nextWriteIndex
        + SCANS_PER_ZONE
        - 1
      ) % SCANS_PER_ZONE;


    uint8_t previousIndex =
      (
        lastIndex
        + SCANS_PER_ZONE
        - 1
      ) % SCANS_PER_ZONE;


    if (
      !z.scans[previousIndex].used
    ) {

      return false;
    }


    out =
      z.scans[previousIndex];


    return true;
  }


  return false;
}


// ================================================================
// ZONE ID
// ================================================================

void zoneIdForCell(
  uint8_t col,
  uint8_t row,
  char *out
) {

  out[0] =
    'A' + col;

  out[1] =
    '1' + row;

  out[2] =
    '\0';
}


// ================================================================
// IDLE SCREEN
// ================================================================

void drawIdleScreen() {

  tft.fillScreen(
    COLOR_BG
  );


  tft.setTextColor(
    COLOR_TEXT
  );

  tft.setTextSize(2);

  tft.setCursor(
    10,
    20
  );

  tft.println(
    F("Crop Sentinel AI")
  );


  tft.setTextSize(1);

  tft.setTextColor(
    COLOR_MUTED
  );

  tft.setCursor(
    10,
    55
  );

  tft.println(
    F("Field Health: 92% - Healthy")
  );


  tft.setCursor(
    10,
    75
  );

  tft.print(
    F("Zones tracked: ")
  );

  tft.println(
    zoneCount
  );


  tft.setTextColor(
    COLOR_TEXT
  );

  tft.setTextSize(2);

  tft.fillRoundRect(
    BTN_X,
    BTN_Y,
    BTN_W,
    BTN_H,
    8,
    COLOR_BUTTON
  );


  tft.setCursor(
    BTN_X + 25,
    BTN_Y + 17
  );

  tft.println(
    F("Scan Crop")
  );


  tft.setTextSize(1);

  tft.setTextColor(
    COLOR_MUTED
  );

  tft.setCursor(
    10,
    315
  );

  tft.println(
    F("Laptop: SCAN:B4")
  );
}


// ================================================================
// ZONE GRID SCREEN
// ================================================================

void drawZonePickScreen() {

  tft.fillScreen(
    COLOR_BG
  );


  tft.setTextColor(
    COLOR_TEXT
  );

  tft.setTextSize(2);

  tft.setCursor(
    10,
    15
  );

  tft.println(
    F("Select Zone")
  );


  tft.setTextSize(1);

  tft.setTextColor(
    COLOR_MUTED
  );

  tft.setCursor(
    10,
    40
  );

  tft.println(
    F("Use laptop: SCAN:A1 to SCAN:E5")
  );


  for (
    uint8_t row = 0;
    row < GRID_ROWS;
    row++
  ) {

    for (
      uint8_t col = 0;
      col < GRID_COLS;
      col++
    ) {

      int x =
        GRID_X +
        col * (
          GRID_CELL +
          GRID_GAP
        );


      int y =
        GRID_Y +
        row * (
          GRID_CELL +
          GRID_GAP
        );


      tft.fillRoundRect(
        x,
        y,
        GRID_CELL,
        GRID_CELL,
        4,
        COLOR_GRID
      );


      char zone[4];

      zoneIdForCell(
        col,
        row,
        zone
      );


      tft.setTextColor(
        COLOR_TEXT
      );

      tft.setTextSize(1);

      tft.setCursor(
        x + 12,
        y + 16
      );

      tft.print(zone);
    }
  }
}


// ================================================================
// SCANNING SCREEN
// ================================================================

void drawScanningScreen(
  const char *zoneId
) {

  tft.fillScreen(
    COLOR_BG
  );


  tft.setTextColor(
    COLOR_TEXT
  );

  tft.setTextSize(2);

  tft.setCursor(
    10,
    25
  );

  tft.print(
    F("Scanning Zone ")
  );

  tft.println(
    zoneId
  );


  tft.setTextSize(1);

  tft.setTextColor(
    COLOR_MUTED
  );

  tft.setCursor(
    10,
    65
  );

  tft.println(
    F("Waiting for ESP32-S3-CAM...")
  );


  tft.setCursor(
    10,
    90
  );

  tft.println(
    F("Image classification in progress")
  );
}


// ================================================================
// RESULT SCREEN
// ================================================================

void drawResultScreen(
  const char *className,
  float confidence,
  bool tinyDiseased,
  bool agree,
  const char *zoneId
) {

  tft.fillScreen(
    COLOR_BG
  );


  bool primaryDiseased =
    strcmp(
      className,
      "healthy"
    ) != 0;


  tft.setTextSize(2);


  tft.setTextColor(
    primaryDiseased
    ? COLOR_DISEASED
    : COLOR_HEALTHY
  );


  tft.setCursor(
    10,
    20
  );


  if (
    primaryDiseased
  ) {

    tft.println(
      className
    );

  } else {

    tft.println(
      F("Healthy")
    );
  }


  tft.setTextSize(1);

  tft.setTextColor(
    COLOR_TEXT
  );


  tft.setCursor(
    10,
    60
  );

  tft.print(
    F("Confidence: ")
  );

  tft.print(
    confidence * 100.0f,
    1
  );

  tft.println(
    F("%")
  );


  tft.setCursor(
    10,
    80
  );

  tft.print(
    F("Zone: ")
  );

  tft.println(
    zoneId
  );


  tft.setCursor(
    10,
    105
  );

  tft.setTextColor(
    agree
    ? COLOR_HEALTHY
    : COLOR_WARNING
  );


  tft.print(
    F("TinyML: ")
  );


  tft.println(
    tinyDiseased
    ? F("DISEASED")
    : F("HEALTHY")
  );


  tft.setCursor(
    10,
    125
  );


  tft.print(
    F("Verification: ")
  );


  tft.println(
    agree
    ? F("AGREE")
    : F("MISMATCH")
  );


  ScanRecord previous;


  if (
    getPreviousReading(
      zoneId,
      previous
    )
  ) {

    tft.setTextColor(
      COLOR_MUTED
    );

    tft.setCursor(
      10,
      155
    );

    tft.println(
      F("Previous reading:")
    );


    tft.setCursor(
      10,
      175
    );

    tft.print(
      previous.className
    );

    tft.print(
      F(" ")
    );

    tft.print(
      previous.confidence * 100.0f,
      1
    );

    tft.println(
      F("%")
    );
  }


  tft.setTextColor(
    COLOR_MUTED
  );

  tft.setCursor(
    10,
    285
  );

  tft.println(
    F("Use HOME or SCAN:A1 from laptop")
  );
}


// ================================================================
// RECEIVE CHARACTER UNTIL TERMINATOR
// ================================================================

bool readUntilChar(
  char terminator,
  char *buffer,
  uint8_t bufferSize,
  unsigned long deadline
) {

  uint8_t index = 0;


  while (
    (long)(
      millis() - deadline
    ) < 0
  ) {

    if (
      espLink.available()
    ) {

      char c =
        espLink.read();


      if (
        c == terminator
      ) {

        if (
          buffer &&
          bufferSize > 0
        ) {

          buffer[
            index < bufferSize - 1
            ? index
            : bufferSize - 1
          ] = '\0';
        }


        return true;
      }


      if (
        buffer &&
        index < bufferSize - 1
      ) {

        buffer[index++] =
          c;
      }
    }
  }


  return false;
}


// ================================================================
// EXPECT STRING
// ================================================================

bool expectLiteral(
  const char *literal,
  unsigned long deadline
) {

  for (
    uint8_t i = 0;
    literal[i] != '\0';
    i++
  ) {

    while (
      !espLink.available()
    ) {

      if (
        (long)(
          millis() - deadline
        ) >= 0
      ) {

        return false;
      }
    }


    if (
      espLink.read()
      != literal[i]
    ) {

      return false;
    }
  }


  return true;
}


// ================================================================
// RECEIVE ESP32 RESULT
//
// Expected format:
//
// {"class":"healthy","confidence":0.94,"feat":[...64 values...]}
// ================================================================

bool receiveScanResult(
  char *className,
  uint8_t classNameSize,
  float &confidence,
  bool &tinyDiseased
) {

  unsigned long deadline =
    millis() + 10000UL;


  // ------------------------------------------------------------
  // {"class":"
  // ------------------------------------------------------------

  if (
    !expectLiteral(
      "{\"class\":\"",
      deadline
    )
  ) {

    return false;
  }


  // ------------------------------------------------------------
  // CLASS
  // ------------------------------------------------------------

  if (
    !readUntilChar(
      '"',
      className,
      classNameSize,
      deadline
    )
  ) {

    return false;
  }


  // ------------------------------------------------------------
  // ","confidence":
  // ------------------------------------------------------------

  if (
    !expectLiteral(
      "\",\"confidence\":",
      deadline
    )
  ) {

    return false;
  }


  char confidenceBuffer[10];


  if (
    !readUntilChar(
      ',',
      confidenceBuffer,
      sizeof(confidenceBuffer),
      deadline
    )
  ) {

    return false;
  }


  confidence =
    atof(
      confidenceBuffer
    );


  // ------------------------------------------------------------
  // "feat":[
  // ------------------------------------------------------------

  if (
    !expectLiteral(
      "\"feat\":[",
      deadline
    )
  ) {

    return false;
  }


  // ------------------------------------------------------------
  // TINYML ACCUMULATOR
  // ------------------------------------------------------------

  int32_t accumulator =
    MODEL_BIAS;


  // ------------------------------------------------------------
  // READ 64 FEATURES
  //
  // We DO NOT store feat[64].
  // This saves RAM.
  // ------------------------------------------------------------

  for (
    uint8_t i = 0;
    i < FEAT_LEN;
    i++
  ) {

    char numberBuffer[8];


    char terminator =
      (
        i < FEAT_LEN - 1
      )
      ? ','
      : ']';


    if (
      !readUntilChar(
        terminator,
        numberBuffer,
        sizeof(numberBuffer),
        deadline
      )
    ) {

      return false;
    }


    int16_t feature =
      (int16_t)atoi(
        numberBuffer
      );


    int16_t weight =
      (int16_t)pgm_read_word(
        &MODEL_WEIGHTS[i]
      );


    accumulator +=
      (
        (int32_t)weight
        *
        (int32_t)feature
      );
  }


  // ------------------------------------------------------------
  // MODEL VERDICT
  // ------------------------------------------------------------

  float score =
    (
      float
    )accumulator
    /
    (
      float
    )MODEL_SCALE;


  tinyDiseased =
    score > 0.0f;


  // ------------------------------------------------------------
  // FINAL }
  // ------------------------------------------------------------

  expectLiteral(
    "}",
    deadline
  );


  return true;
}


// ================================================================
// SEND DASHBOARD RESULT
// ================================================================

void emitDashLine(
  const char *zoneId,
  const char *className,
  float confidence,
  bool tinyDiseased,
  bool agree
) {

  Serial.print(
    F("DASH:{\"zone\":\"")
  );

  Serial.print(
    zoneId
  );

  Serial.print(
    F("\",\"class\":\"")
  );

  Serial.print(
    className
  );

  Serial.print(
    F("\",\"confidence\":")
  );

  Serial.print(
    confidence,
    2
  );

  Serial.print(
    F(",\"tinyVerdict\":\"")
  );

  Serial.print(
    tinyDiseased
    ? F("diseased")
    : F("healthy")
  );

  Serial.print(
    F("\",\"agree\":")
  );

  Serial.print(
    agree
    ? F("true")
    : F("false")
  );

  Serial.print(
    F(",\"ts\":")
  );

  Serial.print(
    millis()
  );

  Serial.println(
    F("}")
  );
}


// ================================================================
// SCAN FLOW
// ================================================================

void doScanFlow(
  const char *zoneId
) {

  currentScreen =
    SCREEN_SCANNING;


  drawScanningScreen(
    zoneId
  );


  // ------------------------------------------------------------
  // Clear old ESP32 data
  // ------------------------------------------------------------

  while (
    espLink.available()
  ) {

    espLink.read();
  }


  // ------------------------------------------------------------
  // Tell ESP32 to scan
  // ------------------------------------------------------------

  espLink.println(
    F("SCAN")
  );


  Serial.print(
    F("[TX -> ESP32] SCAN  (zone ")
  );

  Serial.print(
    zoneId
  );

  Serial.println(
    F(")")
  );


  // ------------------------------------------------------------
  // Receive result
  // ------------------------------------------------------------

  char className[CLASS_NAME_LEN];

  float confidence = 0.0f;

  bool tinyDiseased = false;


  bool received =
    receiveScanResult(
      className,
      sizeof(className),
      confidence,
      tinyDiseased
    );


  // ------------------------------------------------------------
  // SCAN FAILED
  // ------------------------------------------------------------

  if (!received) {

    tft.fillScreen(
      COLOR_BG
    );


    tft.setTextColor(
      COLOR_DISEASED
    );

    tft.setTextSize(2);

    tft.setCursor(
      10,
      25
    );

    tft.println(
      F("Scan Failed")
    );


    tft.setTextColor(
      COLOR_MUTED
    );

    tft.setTextSize(1);

    tft.setCursor(
      10,
      65
    );

    tft.println(
      F("No valid response from ESP32.")
    );


    tft.setCursor(
      10,
      85
    );

    tft.println(
      F("Check ESP32 TX/RX/GND.")
    );


    tft.setCursor(
      10,
      280
    );

    tft.println(
      F("Send HOME to return")
    );


    currentScreen =
      SCREEN_RESULT;


    return;
  }


  // ------------------------------------------------------------
  // PRINT RESULT
  // ------------------------------------------------------------

  Serial.print(
    F("[RX <- ESP32] class=")
  );

  Serial.print(
    className
  );

  Serial.print(
    F(" confidence=")
  );

  Serial.println(
    confidence,
    2
  );


  // ------------------------------------------------------------
  // PRIMARY VERDICT
  // ------------------------------------------------------------

  bool primaryDiseased =
    strcmp(
      className,
      "healthy"
    ) != 0;


  bool agree =
    (
      primaryDiseased
      ==
      tinyDiseased
    );


  // ------------------------------------------------------------
  // HISTORY
  // ------------------------------------------------------------

  addScanToHistory(
    zoneId,
    className,
    confidence
  );


  printZoneHistory(
    zoneId
  );


  // ------------------------------------------------------------
  // DASHBOARD
  // ------------------------------------------------------------

  emitDashLine(
    zoneId,
    className,
    confidence,
    tinyDiseased,
    agree
  );


  // ------------------------------------------------------------
  // TFT RESULT
  // ------------------------------------------------------------

  drawResultScreen(
    className,
    confidence,
    tinyDiseased,
    agree,
    zoneId
  );


  currentScreen =
    SCREEN_RESULT;
}


// ================================================================
// LAPTOP COMMAND HANDLER
// ================================================================
//
// Commands:
//
// SCAN:A1
// SCAN:B4
// SCAN:E5
//
// HOME
//
// ZONES
//
// HELP
//
// ================================================================

bool readLaptopCommand(
  char *zoneId
) {

  static char buffer[20];

  static uint8_t index = 0;


  while (
    Serial.available()
  ) {

    char c =
      Serial.read();


    // ----------------------------------------------------------
    // End of command
    // ----------------------------------------------------------

    if (
      c == '\n' ||
      c == '\r'
    ) {

      if (
        index == 0
      ) {

        continue;
      }


      buffer[index] =
        '\0';


      index = 0;


      // ========================================================
      // SCAN:A1
      // ========================================================

      if (
        strncmp(
          buffer,
          "SCAN:",
          5
        ) == 0
      ) {

        const char *z =
          buffer + 5;


        if (
          z[0] >= 'A' &&
          z[0] <= 'E' &&

          z[1] >= '1' &&
          z[1] <= '5' &&

          z[2] == '\0'
        ) {

          zoneId[0] =
            z[0];

          zoneId[1] =
            z[1];

          zoneId[2] =
            '\0';


          return true;
        }


        Serial.println(
          F("Invalid zone.")
        );

        Serial.println(
          F("Use SCAN:A1 to SCAN:E5")
        );


        return false;
      }


      // ========================================================
      // HOME
      // ========================================================

      if (
        strcmp(
          buffer,
          "HOME"
        ) == 0
      ) {

        currentScreen =
          SCREEN_IDLE;


        drawIdleScreen();


        Serial.println(
          F("[LAPTOP] HOME")
        );


        return false;
      }


      // ========================================================
      // ZONES
      // ========================================================

      if (
        strcmp(
          buffer,
          "ZONES"
        ) == 0
      ) {

        currentScreen =
          SCREEN_ZONE_PICK;


        drawZonePickScreen();


        Serial.println(
          F("[LAPTOP] Zone list displayed.")
        );


        return false;
      }


      // ========================================================
      // HELP
      // ========================================================

      if (
        strcmp(
          buffer,
          "HELP"
        ) == 0
      ) {

        Serial.println(
          F("Available commands:")
        );

        Serial.println(
          F("  SCAN:A1 ... SCAN:E5")
        );

        Serial.println(
          F("  HOME")
        );

        Serial.println(
          F("  ZONES")
        );

        Serial.println(
          F("  HELP")
        );


        return false;
      }


      // ========================================================
      // OLD DASHBOARD COMMAND
      // ========================================================

      if (
        strncmp(
          buffer,
          "DASH_SCAN:",
          10
        ) == 0
      ) {

        const char *z =
          buffer + 10;


        if (
          z[0] >= 'A' &&
          z[0] <= 'E' &&

          z[1] >= '1' &&
          z[1] <= '5' &&

          z[2] == '\0'
        ) {

          zoneId[0] =
            z[0];

          zoneId[1] =
            z[1];

          zoneId[2] =
            '\0';


          return true;
        }
      }


      // ========================================================
      // UNKNOWN COMMAND
      // ========================================================

      Serial.print(
        F("Unknown command: ")
      );

      Serial.println(
        buffer
      );


      Serial.println(
        F("Use SCAN:B4, HOME, ZONES or HELP")
      );


      return false;
    }


    // ----------------------------------------------------------
    // Store character
    // ----------------------------------------------------------

    if (
      index <
      sizeof(buffer) - 1
    ) {

      buffer[index++] =
        c;

    } else {

      index = 0;
    }
  }


  return false;
}


// ================================================================
// SETUP
// ================================================================

void setup() {

  // ------------------------------------------------------------
  // USB SERIAL - LAPTOP
  // ------------------------------------------------------------

  Serial.begin(
    9600
  );


  delay(200);


  Serial.println();

  Serial.println(
    F("=== Crop Sentinel AI -- Phase 6 ===")
  );

  Serial.println(
    F("TFT + Laptop Control + Zone History")
  );


  // ------------------------------------------------------------
  // ESP32 SERIAL
  // ------------------------------------------------------------

  espLink.begin(
    ESP_BAUD
  );


  // ------------------------------------------------------------
  // TFT
  // ------------------------------------------------------------

  tft.begin();


  tft.setRotation(
    2
  );


  tft.fillScreen(
    COLOR_BG
  );


  // ------------------------------------------------------------
  // INITIAL SCREEN
  // ------------------------------------------------------------

  drawIdleScreen();


  currentScreen =
    SCREEN_IDLE;


  Serial.println(
    F("[OK] TFT initialized.")
  );

  Serial.println(
    F("[OK] Idle screen drawn.")
  );


  Serial.println(
    F("")
  );

  Serial.println(
    F("Laptop commands:")
  );

  Serial.println(
    F("  SCAN:A1 ... SCAN:E5")
  );

  Serial.println(
    F("  HOME")
  );

  Serial.println(
    F("  ZONES")
  );

  Serial.println(
    F("  HELP")
  );
}


// ================================================================
// LOOP
// ================================================================

void loop() {

  // ==============================================================
  // LAPTOP CONTROL
  // ==============================================================

  if (
    currentScreen != SCREEN_SCANNING
  ) {

    char laptopZone[4];


    if (
      readLaptopCommand(
        laptopZone
      )
    ) {

      strncpy(
        currentZoneId,
        laptopZone,
        sizeof(currentZoneId) - 1
      );


      currentZoneId[
        sizeof(currentZoneId) - 1
      ] = '\0';


      Serial.print(
        F("[LAPTOP] Starting scan for zone ")
      );

      Serial.println(
        currentZoneId
      );


      doScanFlow(
        currentZoneId
      );
    }
  }


  // ==============================================================
  // RESULT SCREEN
  //
  // No touch is required.
  //
  // Return to HOME using the laptop:
  //
  // HOME
  //
  // ==============================================================

}