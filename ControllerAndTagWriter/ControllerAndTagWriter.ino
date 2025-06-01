#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>

/*
 CONTROLLER CONSTANTS AND VARIABLES.
*/

// Define travel direction or dynamic travel direction mode.
// 0 - North
// 1 - South
// Other: runtime (i.e. using TRAVEL_DIRECTION_PIN)
#define TRAVEL_DIRECTION 0
const byte TRAVEL_DIRECTION_NORTH = 1;
const byte TRAVEL_DIRECTION_SOUTH = 2;
byte travelDirection;

const byte DOOR_ENABLE_NONE = 0;
const byte DOOR_ENABLE_LEFT = 1;
const byte DOOR_ENABLE_RIGHT = 2;
byte doorEnable;

/*
 READER CONSTANTS AND VARIABLES.
*/

// Define to enable developer mode (i.e. writer).
#define DEV_MODE

// PCD: Proximity Coupling Device (reader).
// PICC: Proximity Integrated Circuit Card (tag).

// IO pins.
const byte RIGHT_LED_PIN = 7;
const byte LEFT_LED_PIN = 6;
const byte TRAVEL_DIRECTION_PIN = 5;
const byte MODE_PIN = 4;

// Commonly used constants.
using StatusCode = MFRC522Constants::StatusCode;
using PICC_Type = MFRC522Constants::PICC_Type;
using PICC_Command = MFRC522Constants::PICC_Command;
using PCD_Version = MFRC522Constants::PCD_Version;
using Uid = MFRC522Constants::Uid;
using MIFARE_Key = MFRC522Constants::MIFARE_Key;

// MIFARE Classic 1K: 16 sectors, 4 blocks per sector, 16 bytes per block.
const byte SECTORS_PER_TAG = 16;
const byte BLOCKS_PER_SECTOR = 4;
const byte BYTES_PER_BLOCK = 16;
constexpr byte BYTES_PER_SECTOR = BLOCKS_PER_SECTOR * BYTES_PER_BLOCK;
constexpr byte BYTES_PER_TAG = SECTORS_PER_TAG * BYTES_PER_SECTOR;

// Reader object definitions.
MFRC522DriverPinSimple ssPin(10);
SPIClass &spiClass = SPI;
const SPISettings spiSettings = SPISettings(SPI_CLOCK_DIV4, MSBFIRST, SPI_MODE0);
MFRC522DriverSPI driver{ ssPin, spiClass, spiSettings };
MFRC522 reader{ driver };

// Card key.
MIFARE_Key key;


void setup() {
  // Initialize common components.
  Serial.begin(115200);
  while (!Serial)
    ;

  // Initialize reader.
  bool suc = reader.PCD_Init();
  if (!suc) {
    Serial.println("ERR");
    halt();
  }

  // Initialize factory key.
  for (int8_t i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }

  // Initialize output pins.
  pinMode(LEFT_LED_PIN, OUTPUT);
  pinMode(RIGHT_LED_PIN, OUTPUT);

#ifdef DEV_MODE
  // Enable writer mode.
  pinMode(MODE_PIN, INPUT_PULLUP);

  if (!digitalRead(MODE_PIN)) {
    digitalWrite(LEFT_LED_PIN, HIGH);
    digitalWrite(RIGHT_LED_PIN, HIGH);
    writeFromSerial();
    halt();
  }
#endif

  // Initialize controller
  switch (TRAVEL_DIRECTION) {
    case TRAVEL_DIRECTION_SOUTH:
      travelDirection = TRAVEL_DIRECTION_SOUTH;
      break;
    case TRAVEL_DIRECTION_NORTH:
      travelDirection = TRAVEL_DIRECTION_NORTH;
      break;
    default:
      pinMode(TRAVEL_DIRECTION_PIN, INPUT_PULLUP);
      travelDirection = digitalRead(TRAVEL_DIRECTION_PIN) ? TRAVEL_DIRECTION_NORTH : TRAVEL_DIRECTION_SOUTH;
  }

  doorEnable = DOOR_ENABLE_NONE;
}


void loop() {
  // Loop for controller mode.
  Uid *uid;
  while (!(uid = newMifare1KPresent(reader)))
    ;

  updateControllerWithTag(reader, uid, &key);

  reader.PICC_HaltA();  // Halt the PICC before stopping the encrypted session.
  reader.PCD_StopCrypto1();
}


void updateControllerWithTag(MFRC522 &device, Uid *uid, MIFARE_Key *key) {
  byte sector[BLOCKS_PER_SECTOR][BYTES_PER_BLOCK];

  if (!PICC_ReadMifareClassic1KSector(reader, uid, key, 0, sector)) {
    // TODO: deal with reading faiure.
  }

  if ((sector[1][0] & 0xF0 != 0x10) || (sector[2][0] & 0xF0 != 0x20)) {
    // TODO: deal with invalid block numbers.
  }
  sector[1][0] &= 0x0F;
  sector[2][0] &= 0x0F;

  if (memcmp(sector[1], sector[2], BYTES_PER_BLOCK) != 0) {
    // TODO: deal with reading mismatch;
  }

  // Parse sectors.
  auto b = sector[1];

  bool asdo_enabled = (b[0] & 0x08) != 0;
  byte approach_direction = b[0] & 0x06 >> 1;
  byte version = (b[0] & 0x01 << 1) | (b[1] & 0x80 >> 7);
  byte application_code = b[1] & 0x7F;
  byte csde = b[2] & 0xC0 >> 6;
  uint16_t stop_zone_length = (b[2] & 0x3F << 4) | (b[3] & 0xF0 >> 4);
  uint16_t station_id = (b[3] & 0x0F << 1) | (b[4] << 3) | (b[5] & 0xF0 >> 5);
  uint8_t platform_id = b[5] & 0x0F;
}


Uid *newMifare1KPresent(MFRC522 &device) {
  // Return true when a new Mifare 1K card is detected, false otherwise.
  if (reader.PICC_IsNewCardPresent() && reader.PICC_ReadCardSerial()) {
    auto uid = &(reader.uid);
    if (reader.PICC_GetType(uid->sak) == PICC_Type::PICC_TYPE_MIFARE_1K) {
      return uid;
    }
  }
  return nullptr;
}


bool PICC_ReadMifareClassic1KSector(MFRC522 &device, Uid *uid, MIFARE_Key *key, byte sector, byte outBuffer[][BYTES_PER_BLOCK]) {
  // Based on MFRC522Debug::PICC_DumpMifareClassicSectorToSerial.
  MFRC522::StatusCode status;
  byte firstBlock = sector * BLOCKS_PER_SECTOR;

  // Establish encrypted communications before reading.
  status = device.PCD_Authenticate(PICC_Command::PICC_CMD_MF_AUTH_KEY_A, firstBlock, key, uid);
  if (status != StatusCode::STATUS_OK) {
    Serial.print(F("PCD_Authenticate() failed: "));
    DumpStatusCodeNameToSerial(status);
    Serial.println();
    return false;
  }

  // Dump blocks, highest address first.
  for (int8_t block = BLOCKS_PER_SECTOR - 1; block >= 0; block--) {
    byte blockAddr = firstBlock + block;

    // Read block
    byte buffer[BYTES_PER_BLOCK + 2];  // 2 CRC bits.
    byte byteCount = sizeof(buffer);
    status = device.MIFARE_Read(blockAddr, buffer, &byteCount);
    if (status != StatusCode::STATUS_OK) {
      Serial.print(F("MIFARE_Read() failed: "));
      DumpStatusCodeNameToSerial(status);
      Serial.println();
      return false;
    }

    // Parse sector trailer data
    if (block + 1 == BLOCKS_PER_SECTOR) {
      // The access bits are stored in a peculiar fashion.
      // There are four groups:
      //		g[3]	Access bits for the sector trailer, block 3.
      //		g[2]	Access bits for block 2.
      //		g[1]	Access bits for block 1.
      //		g[0]	Access bits for block 0.
      // Each group has access bits [C1 C2 C3]. In this code C1 is MSB and C3 is LSB.
      // The four CX bits are stored together in a nible cx and an inverted nible cx_.
      byte c1, c2, c3;     // Nibbles.
      byte c1_, c2_, c3_;  // Inverted nibbles.
      byte g[4];           // Access bits for each of the four groups.

      c1 = buffer[7] >> 4;
      c2 = buffer[8] & 0xF;
      c3 = buffer[8] >> 4;
      c1_ = buffer[6] & 0xF;
      c2_ = buffer[6] >> 4;
      c3_ = buffer[7] & 0xF;
      if ((c1 != (~c1_ & 0xF)) || (c2 != (~c2_ & 0xF)) || (c3 != (~c3_ & 0xF))) {
        // Inverted error.
        Serial.println(F("Inverted access bits did not match!"));
        return false;
      }

      g[0] = ((c1 & 1) << 2) | ((c2 & 1) << 1) | ((c3 & 1) << 0);
      g[1] = ((c1 & 2) << 1) | ((c2 & 2) << 0) | ((c3 & 2) >> 1);
      g[2] = ((c1 & 4) << 0) | ((c2 & 4) >> 1) | ((c3 & 4) >> 2);
      g[3] = ((c1 & 8) >> 1) | ((c2 & 8) >> 2) | ((c3 & 8) >> 3);
    }

    for (int8_t i = 0; i < BYTES_PER_BLOCK; i++) {
      outBuffer[block][i] = buffer[i];
    }
  }

  return true;
}


#ifdef DEV_MODE
void writeFromSerial() {
  // Write to Sector 0, blocks 1 and 2, from data received from Serial.

  // Ready signal.
  Serial.println("WRITER READY");

  // Receives and echoes block to write.
  byte blockBytes[BYTES_PER_BLOCK] = {};
  Serial.readBytes(blockBytes, BYTES_PER_BLOCK);
  Serial.write(blockBytes, BYTES_PER_BLOCK);

  // Waits for OK signal.
  while (!Serial.available())
    ;
  if (Serial.read() != 0x40) return;

  // Wait for tag.
  while (!newMifare1KPresent(reader))
    ;

  // Authenticate.
  auto authStatus = reader.PCD_Authenticate(PICC_Command::PICC_CMD_MF_AUTH_KEY_A, 0, &key, &(reader.uid));
  if (authStatus != StatusCode::STATUS_OK) {
    Serial.print(F("Authenticate failed: "));
    DumpStatusCodeNameToSerial(authStatus);
    Serial.println();
    return;
  }

  // Write to blocks 1 and 2.
  for (int i = 1; i <= 2; i++) {
    // Sets the block number in the first byte.
    byte fb = blockBytes[0];
    fb &= 0x0F;
    fb |= i << 4 & 0xF0;
    blockBytes[0] = fb;

    auto writeStatus = reader.MIFARE_Write(i, &blockBytes[0], BYTES_PER_BLOCK);
    if (writeStatus != StatusCode::STATUS_OK) {
      Serial.print(F("Write (block "));
      Serial.print(i);
      Serial.print(F(") failed: "));
      DumpStatusCodeNameToSerial(writeStatus);
      Serial.println();
      return;
    }
  }

  // Write done.
  Serial.println("WRITE DONE");
}
#endif


void DumpStatusCodeNameToSerial(StatusCode code) {
  // Based on MFRC522Debug::PICC_GetTypeName.
  switch (code) {
    case StatusCode::STATUS_OK:
      Serial.println(F("Success."));
      break;
    case StatusCode::STATUS_ERROR:
      Serial.println(F("Error in communication."));
      break;
    case StatusCode::STATUS_COLLISION:
      Serial.println(F("Collision detected."));
      break;
    case StatusCode::STATUS_TIMEOUT:
      Serial.println(F("Timeout in communication."));
      break;
    case StatusCode::STATUS_NO_ROOM:
      Serial.println(F("A buffer is not big enough."));
      break;
    case StatusCode::STATUS_INTERNAL_ERROR:
      Serial.println(F("Internal error in the code. Should not happen."));
      break;
    case StatusCode::STATUS_INVALID:
      Serial.println(F("Invalid argument."));
      break;
    case StatusCode::STATUS_CRC_WRONG:
      Serial.println(F("The CRC_A does not match."));
      break;
    case StatusCode::STATUS_MIFARE_NACK:
      Serial.println(F("A MIFARE PICC responded with NAK."));
      break;
    default:
      Serial.println(F("Unknown error."));
      break;
  }
}


void halt() {
  // Halt and blink lights.
  bool tgl = true;
  while (true) {
    digitalWrite(LEFT_LED_PIN, tgl);
    digitalWrite(RIGHT_LED_PIN, !tgl);
    delay(200);
    tgl = !tgl;
  }
}
