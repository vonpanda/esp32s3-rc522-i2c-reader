/*
 * YFROBOT RFID IIC 通讯示例：打印卡信息
 * 
 * 本示例程序用于通过 IIC 通讯接口读取 RFID 卡的信息并打印到串口监视器。
 * 包括卡的 UID、类型和数据块信息。
 * 
 * 作者: YFROBOTZL
 * 日期: 2024-11-17
 * 链接: http://www.yfrobot.com.cn
 */

#include <Wire.h>
#include "YFROBOTRFIDI2C.h"
YFROBOTRFID yf_rfid;
YFROBOTRFID::StatusCode status;

void setup() {
  Serial.begin(9600);  // Initialize serial communications with the PC
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

  // Now a card is selected. The UID and SAK is in yf_rfid.uid. 打印输出 UID
  Serial.print(F("Card UID:"));
  // Serial.print(yf_rfid.uid);
  for (byte i = 0; i < yf_rfid.uid.size; i++) {
    Serial.print(yf_rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(yf_rfid.uid.uidByte[i], HEX);
  }
  Serial.print(" , ");

  /* 打印输出 卡类型 */
  Serial.print(F("PICC type:"));
  Serial.println(yf_rfid.PICC_GetTypeName(yf_rfid.PICC_GetType(yf_rfid.uid.sak)));

  // Dump debug info about the card; PICC_HaltA() is automatically called
  // 打印卡相关调试信息；自动调用PICC_HaltA()停止重复读卡
  yf_rfid.PICC_DumpToSerial(&(yf_rfid.uid));
}
