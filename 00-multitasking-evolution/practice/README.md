# ปฏิบัติการ: วิวัฒนาการของ Multitasking

## ภาพรวม
การปฏิบัติการนี้จะแสดงให้เห็นความแตกต่างระหว่างเทคนิค Multitasking ต่างๆ ผ่านการเขียนโปรแกรมจริง

## 📋 รายการแลป

### Lab 1: Single Task vs Multitasking Demo (45 นาที)
- **ไฟล์**: [lab1-single-vs-multi/](lab1-single-vs-multi/)
- **เป้าหมาย**: เปรียบเทียบระบบ Single Task กับ Multitasking
- **กิจกรรม**:
  - สร้างระบบ Single Task ที่ควบคุม LED และอ่าน Sensor
  - แปลงเป็น Multitasking แบบง่าย
  - สังเกตความแตกต่างในการตอบสนอง

  ระบบ Multitasking สามารถตอบสนองต่อปุ่มและทำงานหลายอย่างพร้อมกันได้ ในขณะที่ Single Task ต้องรอการประมวลผลจบก่อน

### Lab 2: Time-Sharing Implementation (45 นาที)
- **ไฟล์**: [lab2-time-sharing/](lab2-time-sharing/)
- **เป้าหมาย**: ทำความเข้าใจ Time-Sharing และปัญหาที่เกิดขึ้น
- **กิจกรรม**:
  - สร้างระบบ Time-Sharing แบบ manual
  - ทดสอบปัญหา Context Switching Overhead
  - วัด CPU utilization

Time slice ที่เหมาะสมที่สุดอยู่ระหว่าง 50–100 ms เพราะให้ประสิทธิภาพสูงสุดโดยไม่เกิด overhead มากเกินไป

### Lab 3: Cooperative vs Preemptive Comparison (30 นาที)
- **ไฟล์**: [lab3-coop-vs-preemptive/](lab3-coop-vs-preemptive/)
- **เป้าหมาย**: เปรียบเทียบ Cooperative กับ Preemptive Multitasking
- **กิจกรรม**:
  - ทดสอบ Cooperative Multitasking
  - ทดสอบ Preemptive Multitasking (ใช้ FreeRTOS)
  - วิเคราะห์ข้อดี-ข้อเสีย

  Cooperative: โค้ดง่าย แต่ task หนึ่งอาจครอบ CPU ทำให้ระบบช้า

Preemptive: ตอบสนองเร็วกว่า มี priority ชัด แต่ต้องออกแบบระมัดระวังเรื่อง resource sharing

## 🛠️ เครื่องมือที่ใช้
- ESP32 Development Board
- ESP-IDF
- VS Code
- Logic Analyzer หรือ Oscilloscope (ถ้ามี)

## 📊 การประเมินผล
- การทำงานของโปรแกรม (60%)
- การวิเคราะห์และเปรียบเทียบ (30%)
- การนำเสนอผลการทดลอง (10%)

## 📝 รายงานการทดลอง
ให้สร้างไฟล์ `report.md` สำหรับบันทึกผลการทดลองและการวิเคราะห์