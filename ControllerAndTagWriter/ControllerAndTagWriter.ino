#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>

/*
 CONTROLLER CONSTANTS AND VARIABLES.
*/

// Define travel direction or dynamic travel direction mode.
// 0b01 - North
// 0b10 - South
// Other: runtime (i.e. using TRAVEL_DIRECTION_PIN)
#define TRAVEL_DIRECTION TRAVEL_DIRECTION_DYNAMIC
const uint8_t TRAVEL_DIRECTION_DYNAMIC = 0b00;
const uint8_t TRAVEL_DIRECTION_NORTH = 0b01;
const uint8_t TRAVEL_DIRECTION_SOUTH = 0b10;
uint8_t travelDirection;

const uint8_t DOOR_ENABLE_NONE = 0b00;
const uint8_t DOOR_ENABLE_LEFT = 0b01;
const uint8_t DOOR_ENABLE_RIGHT = 0b10;
constexpr uint8_t DOOR_ENABLE_BOTH = DOOR_ENABLE_LEFT | DOOR_ENABLE_RIGHT;

/*
 READER CONSTANTS AND VARIABLES.
*/

// Define to enable developer mode (i.e. writer).
#define DEV_MODE

// PCD: Proximity Coupling Device (reader).
// PICC: Proximity Integrated Circuit Card (tag).

// IO pins.
const uint8_t RIGHT_LED_PIN = 7;
const uint8_t LEFT_LED_PIN = 6;
const uint8_t TRAVEL_DIRECTION_PIN = 5;
const uint8_t MODE_PIN = 4;

// Commonly used constants.
using StatusCode = MFRC522Constants::StatusCode;
using PICC_Type = MFRC522Constants::PICC_Type;
using PICC_Command = MFRC522Constants::PICC_Command;
using PCD_Version = MFRC522Constants::PCD_Version;
using Uid = MFRC522Constants::Uid;
using MIFARE_Key = MFRC522Constants::MIFARE_Key;

// MIFARE Classic 1K: 16 sectors, 4 blocks per sector, 16 bytes per block.
const uint8_t SECTORS_PER_TAG = 16;
const uint8_t BLOCKS_PER_SECTOR = 4;
const uint8_t BYTES_PER_BLOCK = 16;
constexpr uint8_t BYTES_PER_SECTOR = BLOCKS_PER_SECTOR * BYTES_PER_BLOCK;
constexpr uint8_t BYTES_PER_TAG = SECTORS_PER_TAG * BYTES_PER_SECTOR;

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

  // Initialize output pins.
  pinMode(LEFT_LED_PIN, OUTPUT);
  pinMode(RIGHT_LED_PIN, OUTPUT);
  pinMode(MODE_PIN, INPUT_PULLUP);

  // Initialize reader.
  bool suc = reader.PCD_Init();
  if (!suc) {
    Serial.println("ERR");
    halt();
  }
  reader.PICC_HaltA();
  reader.PCD_StopCrypto1();

  // Initialize factory key.
  for (int8_t i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }

#ifdef DEV_MODE
  // Enable writer mode.
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

  Serial.print(F("Travel direction: "));
  switch (travelDirection) {
    case TRAVEL_DIRECTION_NORTH:
      Serial.println("North");
      break;
    case TRAVEL_DIRECTION_SOUTH:
      Serial.println("South");
      break;
    default:
      Serial.println("Invalid");
  }
}


void loop() {
  // Loop for controller mode.
  Uid *uid = nullptr;
  while (!(uid = newMifare1KPresent(reader)))
    ;

  uint8_t doorEnable;
  if (updateControllerWithTag(reader, &(reader.uid), &key, doorEnable)) {
    digitalWrite(LEFT_LED_PIN, doorEnable & DOOR_ENABLE_LEFT);
    digitalWrite(RIGHT_LED_PIN, doorEnable & DOOR_ENABLE_RIGHT);
  }

  reader.PICC_HaltA();  // Halt the PICC before stopping the encrypted session.
  reader.PCD_StopCrypto1();
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


bool updateControllerWithTag(MFRC522 &device, Uid *uid, MIFARE_Key *key, uint8_t &doorEnable) {
  // Read tag. When applicable, update doorEnabled and return true.

  uint8_t sector[BLOCKS_PER_SECTOR][BYTES_PER_BLOCK];
  if (!PICC_ReadMifareClassic1KSector(reader, uid, key, 0, sector, 5)) {
    Serial.println("Failed to read tag.");
    // TODO: make tag reading more reliable.
    // TODO: deal with new tags detected as they exit zone.
    // doorEnable = DOOR_ENABLE_NONE;
    // return true;
    return false;
  }

  if ((sector[1][0] & 0xF0 != 0x10) || (sector[2][0] & 0xF0 != 0x20)) {
    Serial.println("Invalid block numbers.");
    doorEnable = DOOR_ENABLE_NONE;
    return true;
  }
  sector[1][0] &= 0x0F;
  sector[2][0] &= 0x0F;

  if (memcmp(sector[1], sector[2], BYTES_PER_BLOCK) != 0) {
    Serial.println("Blocks don't match.");
    doorEnable = DOOR_ENABLE_NONE;
    return true;
  }

  // Parse sectors.
  auto b = sector[1];

  bool asdoEnabled = (b[0] & 0x08) != 0;
  uint8_t approachDirection = (b[0] & 0x06) >> 1;
  uint8_t version = ((b[0] & 0x01) << 1) | ((b[1] & 0x80) >> 7);
  uint8_t applicationCode = b[1] & 0x7F;
  uint8_t csde = (b[2] & 0xC0) >> 6;
  uint16_t stopZoneLength = ((b[2] & 0x3F) << 4) | ((b[3] & 0xF0) >> 4);
  uint16_t stationId = ((b[3] & 0x0F) << 1) | (b[4] << 3) | ((b[5] & 0xF0) >> 5);
  uint8_t platformId = b[5] & 0x0F;

  // Dump sectors to Serial.
  Serial.print(F("Block read: "));
  for (int i = 0; i < BYTES_PER_BLOCK; i++) {
    if (b[i] < 0x10) Serial.print("0");
    Serial.print(b[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  Serial.print(F("ASDO Enabled: "));
  Serial.println(asdoEnabled);

  Serial.print(F("Approach direction: "));
  switch (approachDirection) {
    case TRAVEL_DIRECTION_NORTH:
      Serial.println("North");
      break;
    case TRAVEL_DIRECTION_SOUTH:
      Serial.println("South");
      break;
    default:
      Serial.println("Invalid");
  }

  Serial.print(F("Version: "));
  Serial.println(version);

  Serial.print(F("Application code: "));
  Serial.println(applicationCode);

  Serial.print(F("CSDE: "));
  switch (csde) {
    case DOOR_ENABLE_NONE:
      Serial.println("None");
      break;
    case DOOR_ENABLE_LEFT:
      Serial.println("Left");
      break;
    case DOOR_ENABLE_RIGHT:
      Serial.println("Right");
      break;
    case DOOR_ENABLE_BOTH:
      Serial.println("Both");
      break;
    default:
      Serial.println("Invalid");
  }

  Serial.print(F("Stop zone length: "));
  Serial.println(stopZoneLength);

  Serial.print(F("Station ID: "));
  Serial.println(stationId);

  Serial.print(F("Platform ID: "));
  Serial.println(platformId);

  // Verify if tag is applicable.
  if (!asdoEnabled
      || approachDirection != travelDirection
      || version != 0
      || applicationCode != 1) {
    return false;
  }

  // Update door enable.
  if (stopZoneLength > 0) {
    doorEnable = csde;
  } else {
    doorEnable = DOOR_ENABLE_NONE;
  }
  return true;
}


bool PICC_ReadMifareClassic1KSector(
  MFRC522 &device,
  Uid *uid,
  MIFARE_Key *key,
  byte sector, byte outBuffer[][BYTES_PER_BLOCK],
  uint8_t maxRetries) {
  // Based on MFRC522Debug::PICC_DumpMifareClassicSectorToSerial.

  MFRC522::StatusCode status;
  int i;
  uint8_t firstBlock = sector * BLOCKS_PER_SECTOR;

  // Establish encrypted communications before reading.
  for (i = 1; i <= maxRetries; i++) {
    status = device.PCD_Authenticate(PICC_Command::PICC_CMD_MF_AUTH_KEY_A, firstBlock, key, uid);
    if (status == StatusCode::STATUS_OK) {
      break;
    }
    if (i == maxRetries) {
      return false;
    }
    delay(100);
  }

  // Dump blocks, highest address first.
  for (int8_t block = BLOCKS_PER_SECTOR - 1; block >= 0; block--) {
    uint8_t blockAddr = firstBlock + block;

    // Read block
    uint8_t buffer[BYTES_PER_BLOCK + 2];  // 2 CRC bits.
    uint8_t byteCount = sizeof(buffer);

    for (i = 1; i <= maxRetries; i++) {
      status = device.MIFARE_Read(blockAddr, buffer, &byteCount);
      if (status == StatusCode::STATUS_OK) {
        break;
      }
      if (i == maxRetries) {
        device.PICC_HaltA();
        device.PCD_StopCrypto1();
        return false;
      }
      delay(100);
    }

    // Copy block to output buffer.
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
  uint8_t blockBytes[BYTES_PER_BLOCK] = {};
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
    uint8_t fb = blockBytes[0];
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
