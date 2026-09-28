/*
 * YFROBOT RFID IIC 通讯示例：读取卡数据
 * 注意：此例程结合“写入卡数据”例程使用。
 * 
 * 本示例程序通过 IIC 通讯接口将数据写入 RFID 卡的指定数据块。
 * 程序首先检测是否有新的卡片进入读卡范围，然后选择卡片并写入数据。
 * 写入完成后，程序会打印写入结果到串口监视器。
 * 
 * 作者: YFROBOTZL
 * 日期: 2024-11-17
 * 链接: http://www.yfrobot.com.cn
 */

#include <Wire.h>
#include "YFROBOTRFIDI2C.h"
YFROBOTRFID yf_rfid;
YFROBOTRFID::StatusCode status;

byte block = 4;  // 读取的数据块

void setup() {
  Serial.begin(9600);   // Initialize serial communications with the PC
  Wire.begin();         // Initialize I2C
  yf_rfid.PCD_Init();   // Init YFROBOT RFID
  String ver = yf_rfid.GetReaderVersion();
  Serial.println(ver);  // YFROBOT RFID Card Reader ver
  Serial.println(F("Scan PICC to see UID, type, and data blocks..."));
}

void loop() {
  // Look for new cards, and select one if present
  if (!yf_rfid.PICC_IsNewCardPresent() || !yf_rfid.PICC_ReadCardSerial()) {
    delay(50);
    return;
  }

  // 读卡测试中，读取指定位置数据
  byte readValue[18];
  byte len = 18;
  // 尝试使用Key A对块进行身份验证
  status = (YFROBOTRFID::StatusCode)yf_rfid.PCD_Authenticate(YFROBOTRFID::PICC_CMD_MF_AUTH_KEY_A, block, &yf_rfid.keyA, &(yf_rfid.uid));
  if (status != YFROBOTRFID::STATUS_OK) {
    Serial.print(F("Authentication failed: "));
    Serial.println(yf_rfid.GetStatusCodeName(status));
    return;
  }
  // 读出块数据
  status = (YFROBOTRFID::StatusCode)yf_rfid.MIFARE_Read(block, readValue, &len);
  if (status != YFROBOTRFID::STATUS_OK) {
    Serial.print(F("Reading failed: "));
    Serial.println(yf_rfid.GetStatusCodeName(status));
    return;
  }
  // 打印数据
  String value = "";
  for (uint8_t i = 0; i < 16; i++) {
    value += (char)readValue[i];
  }
  value.trim();  // 去除字符串两端的空白字符
  Serial.println("Read data: ");
  Serial.println(value);
  Serial.println(F("**End Reading**"));

  yf_rfid.PICC_HaltA();       // 指示处于 ACTIVE(*) 状态的 PICC 进入 HALT 状态。
  yf_rfid.PCD_StopCrypto1();  // 用于使 PCD 从其已验证状态退出。
}
