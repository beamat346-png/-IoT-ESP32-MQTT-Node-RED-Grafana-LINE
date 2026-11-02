-- รันครั้งเดียวด้วยผู้ใช้ root:  mysql -u root -p < schema.sql
CREATE DATABASE IF NOT EXISTS iot_db CHARACTER SET utf8mb4;
USE iot_db;

CREATE TABLE IF NOT EXISTS readings (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  node_id VARCHAR(32),
  temperature FLOAT,
  humidity FLOAT,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  INDEX (created_at)
);

-- ผู้ใช้สำหรับ Node-RED (เขียนอย่างเดียว) -- เปลี่ยนรหัสผ่านก่อนใช้งานจริง
CREATE USER IF NOT EXISTS 'nodered'@'localhost' IDENTIFIED BY 'CHANGE_ME_NODERED';
GRANT INSERT ON iot_db.readings TO 'nodered'@'localhost';

-- ผู้ใช้สำหรับ Grafana (อ่านอย่างเดียว) -- เปลี่ยนรหัสผ่านก่อนใช้งานจริง
CREATE USER IF NOT EXISTS 'grafana'@'localhost' IDENTIFIED BY 'CHANGE_ME_GRAFANA';
GRANT SELECT ON iot_db.readings TO 'grafana'@'localhost';

FLUSH PRIVILEGES;
