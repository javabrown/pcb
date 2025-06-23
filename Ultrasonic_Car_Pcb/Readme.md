### Ultrasonic car PCB design:

---

#### 🛠️ Controller:
- ESP32-CAM (WROVER-KIT) mounted centrally with breakout GPIO headers
- UART breakout (TX/RX/GND) for flashing

#### ⚡ Power:
- Battery input: 7.4V (Li-ion) or 9V battery via JST plug or screw terminal
- Onboard buck converter module (footprint provided) to convert to 5V
- 3.3V LDO regulator circuit for ESP32 logic
- Power filtering capacitors and reverse polarity diode

#### 🚗 DC Motor Driver:
- L298N dual H-bridge driver
- Motor A/B screw terminals
- ENA/ENB and IN1-IN4 routed to ESP32 GPIO headers

#### 🛰 Ultrasonic Sensors:
- 4x HC-SR04 headers (Front, Back, Left, Right)
- Each with labeled 4-pin header: VCC, GND, Trig, Echo

#### ⚙️ Servo Mounts:
- 2x SG90/MG90 Servo headers (Front, Rear)
- PWM signal connected to ESP32 GPIO
- 5V and GND lines filtered with capacitors

#### 🛋 Board Design:
- Silkscreen labels for all components and pins
- Screw hole pads on 4 corners for car chassis mounting
- Clean copper routing with wide power traces for motors
- 3D view verified


