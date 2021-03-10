#include "floower.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#define LOG_TAG ""
#else
#include "esp_log.h"
static const char* LOG_TAG = "Floower";
#endif

#define NEOPIXEL_PIN 27
#define NEOPIXEL_PWR_PIN 25

#define SERVO_PIN 26
#define SERVO_PWR_PIN 33
#define SERVO_POWER_OFF_DELAY 500

#define TOUCH_SENSOR_PIN 4
#define TOUCH_FADE_TIME 75 // 50
#define TOUCH_LONG_TIME_THRESHOLD 2000 // 2s to recognize long touch
#define TOUCH_HOLD_TIME_THRESHOLD 5000 // 5s to recognize hold touch
#define TOUCH_COOLDOWN_TIME 300 // prevent random touch within 300ms after last touch

#define BATTERY_ANALOG_PIN 36 // VP
#define USB_ANALOG_PIN 39 // VN

#define ACTY_LED_PIN 2
#define ACTY_BLINK_TIME 50

unsigned long Floower::touchStartedTime = 0;
unsigned long Floower::touchEndedTime = 0;
unsigned long Floower::lastTouchTime = 0;

const RgbColor candleColor(178, 45, 0); // 36

Floower::Floower(Config *config) : config(config), animations(2), pixels(7, NEOPIXEL_PIN) {}

void Floower::init() {
  // LEDs
  pixelsPowerOn = true; // to make setPixelsPowerOn effective
  setPixelsPowerOn(false);
  pinMode(NEOPIXEL_PWR_PIN, OUTPUT);

  pixelsColor = colorBlack;
  RgbColor pixelsOriginColor = colorBlack;
  RgbColor pixelsTargetColor = colorBlack;
  pixels.Begin();
  showColor(pixelsColor);
  pixels.Show();

  // configure ADC for battery level reading
  analogReadResolution(12); // se0t 12bit resolution (0-4095)
  analogSetAttenuation(ADC_11db); // set AREF to be 3.6V
  analogSetCycles(8); // num of cycles per sample, 8 is default optimal
  analogSetSamples(1); // num of samples

  // acty LED
  pinMode(ACTY_LED_PIN, OUTPUT);
  digitalWrite(ACTY_LED_PIN, HIGH);

  // this need to be done in init in order to enable deep sleep wake up
  enableTouch();
}

void Floower::initServo() {
  // default servo configuration
  servoOpenAngle = config->servoOpen;
  servoClosedAngle = config->servoClosed;
  servoAngle = config->servoClosed;
  servoOriginAngle = config->servoClosed;
  servoTargetAngle = config->servoClosed;
  petalsOpenLevel = -1; // 0-100% (-1 unknown)

  // servo
  servoPowerOn = true; // to make setServoPowerOn effective
  setServoPowerOn(false);
  pinMode(SERVO_PWR_PIN, OUTPUT);

  servo.setPeriodHertz(50); // standard 50 Hz servo
  if (config->calibrated) {
    servo.attach(SERVO_PIN, servoClosedAngle, servoOpenAngle);
  }
  else {
    servo.attach(SERVO_PIN); // DANGER! no boundaries to allow calibration
  }
  servo.write(servoAngle);
}

void Floower::update() {
  animations.UpdateAnimations();
  unsigned long now = millis();
  handleTimers(now);

  // show pixels
  if (pixelsColor.CalculateBrightness() > 0) {
    setPixelsPowerOn(true);
    pixels.Show();
  }
  else {
    pixels.Show();
    setPixelsPowerOn(false);
  }

  if (touchStartedTime > 0) {
    unsigned int touchTime = now - touchStartedTime;
    unsigned long sinceLastTouch = millis() - lastTouchTime;

    if (!touchRegistered) {
      ESP_LOGD(LOG_TAG, "Touch Down");
      touchRegistered = true;
      if (touchCallback != nullptr) {
        touchCallback(FloowerTouchEvent::TOUCH_DOWN);
      }
    }
    if (!longTouchRegistered && touchTime > TOUCH_LONG_TIME_THRESHOLD) {
      ESP_LOGD(LOG_TAG, "Long Touch %d", touchTime);
      longTouchRegistered = true;
      if (touchCallback != nullptr) {
        touchCallback(FloowerTouchEvent::TOUCH_LONG);
      }
    }
    if (!holdTouchRegistered && touchTime > TOUCH_HOLD_TIME_THRESHOLD) {
      ESP_LOGD(LOG_TAG, "Hold Touch %d", touchTime);
      holdTouchRegistered = true;
      if (touchCallback != nullptr) {
        touchCallback(FloowerTouchEvent::TOUCH_HOLD);
      }
    }
    if (sinceLastTouch > TOUCH_FADE_TIME) {
      ESP_LOGD(LOG_TAG, "Touch Up %d", sinceLastTouch);
      touchStartedTime = 0;
      touchEndedTime = now;
      touchRegistered = false;
      longTouchRegistered = false;
      holdTouchRegistered = false;
      if (touchCallback != nullptr) {
        touchCallback(FloowerTouchEvent::TOUCH_UP);
      }
    }
  }
  else if (touchEndedTime > 0 && millis() - touchEndedTime > TOUCH_COOLDOWN_TIME) {
    ESP_LOGD(LOG_TAG, "Touch enabled");
    touchEndedTime = 0;
  }
}

void Floower::registerOutsideTouch() {
  touchISR();
}

void Floower::enableTouch() {
  detachInterrupt(TOUCH_SENSOR_PIN);
  touchAttachInterrupt(TOUCH_SENSOR_PIN, Floower::touchISR, config->settings.touchThreshold);
}

void Floower::touchISR() {
  lastTouchTime = millis();
  if (touchStartedTime == 0 && touchEndedTime == 0) {
    touchStartedTime = lastTouchTime;
  }
}

void Floower::onLeafTouch(FloowerOnLeafTouchCallback callback) {
  touchCallback = callback;
}

void Floower::onChange(FloowerChangeCallback callback) {
  changeCallback = callback;
}

void Floower::setPetalsOpenLevel(uint8_t level, int transitionTime) {
  if (level == petalsOpenLevel) {
    return; // no change, keep doing the old movement until done
  }
  petalsOpenLevel = level;

  if (level >= 100) {
    setPetalsAngle(servoOpenAngle, transitionTime);
  }
  else {
    float position = (servoOpenAngle - servoClosedAngle);
    position = position * level / 100.0;
    setPetalsAngle(servoClosedAngle + position, transitionTime);
  }
}

void Floower::setPetalsAngle(unsigned int angle, int transitionTime) {
  servoOriginAngle = servoAngle;
  servoTargetAngle = angle;

  ESP_LOGI(LOG_TAG, "Petals %d%% (%d) in %d", petalsOpenLevel, angle, transitionTime);

  // TODO: support transitionTime of 0
  animations.StartAnimation(0, transitionTime, [=](const AnimationParam& param){ servoAnimationUpdate(param); });

  if (changeCallback != nullptr) {
    changeCallback(petalsOpenLevel, pixelsTargetColor);
  }
}

void Floower::servoAnimationUpdate(const AnimationParam& param) {
  servoAngle = servoOriginAngle + (servoTargetAngle - servoOriginAngle) * param.progress;

  setServoPowerOn(true);
  servo.write(servoAngle);

  if (param.state == AnimationState_Completed) {
    servoPowerOffTime = millis() + SERVO_POWER_OFF_DELAY;
  }
}

uint8_t Floower::getPetalsOpenLevel() {
  return petalsOpenLevel;
}

uint8_t Floower::getCurrentPetalsOpenLevel() {
  if (arePetalsMoving()) {
    float range = servoOpenAngle - servoClosedAngle;
    float current = servoAngle - servoClosedAngle;
    return (current / range) * 100;
  }
  return petalsOpenLevel;
}

int Floower::getPetalsAngle() {
  return servoTargetAngle;
}

int Floower::getCurrentPetalsAngle() {
  return servoAngle;
}

void Floower::setColor(RgbColor color, FloowerColorMode colorMode, int transitionTime) {
  if (color.R == pixelsTargetColor.R && color.G == pixelsTargetColor.G && color.B == pixelsTargetColor.B && pixelsColorMode == colorMode) {
    return; // no change
  }

  pixelsColorMode = colorMode;
  pixelsTargetColor = color;
  interruptiblePixelsAnimation = false;

  ESP_LOGI(LOG_TAG, "Color %d,%d,%d", color.R, color.G, color.B);

  if (transitionTime <= 0) {
    pixelsColor = color;
    showColor(pixelsColor);
  }
  else if (colorMode == TRANSITION) {
    pixelsOriginColor = pixelsColor;
    animations.StartAnimation(1, transitionTime, [=](const AnimationParam& param){ pixelsTransitionAnimationUpdate(param); });  
  }
  else if (colorMode == FLASH) {
    pixelsOriginColor = colorBlack; //RgbColor::LinearBlend(color, colorBlack, 0.95);
    animations.StartAnimation(1, transitionTime, [=](const AnimationParam& param){ pixelsFlashAnimationUpdate(param); });
  }

  if (changeCallback != nullptr) {
    changeCallback(petalsOpenLevel, pixelsTargetColor);
  }
}

void Floower::pixelsTransitionAnimationUpdate(const AnimationParam& param) {
  pixelsColor = RgbColor::LinearBlend(pixelsOriginColor, pixelsTargetColor, param.progress);
  showColor(pixelsColor);
}

void Floower::pixelsFlashAnimationUpdate(const AnimationParam& param) {
  if (param.progress < 0.5) {
    float progress = NeoEase::CubicInOut(param.progress * 2);
    pixelsColor = RgbColor::LinearBlend(pixelsOriginColor, pixelsTargetColor, progress);
  }
  else {
    float progress = NeoEase::CubicInOut((1 - param.progress) * 2);
    pixelsColor = RgbColor::LinearBlend(pixelsOriginColor, pixelsTargetColor, progress);
  }

  showColor(pixelsColor);

  if (param.state == AnimationState_Completed) {
    if (pixelsTargetColor.CalculateBrightness() > 0) { // while there is something to show
      animations.RestartAnimation(param.index);
    }
  }
}

RgbColor Floower::getColor() {
  return pixelsTargetColor;
}

RgbColor Floower::getCurrentColor() {
  return pixelsColor;
}

void Floower::startAnimation(FloowerColorAnimation animation) {
  interruptiblePixelsAnimation = true;

  if (animation == RAINBOW) {
    pixelsOriginColor = pixelsColor;
    animations.StartAnimation(1, 10000, [=](const AnimationParam& param){ pixelsRainbowAnimationUpdate(param); });
  }
  else if (animation == CANDLE) {
    pixelsTargetColor = pixelsColor = RgbColor(candleColor); // candle orange
    for (uint8_t i = 0; i < 6; i++) {
      candleOriginColors[i] = pixelsTargetColor;
      candleTargetColors[i] = pixelsTargetColor;
    }
    animations.StartAnimation(1, 100, [=](const AnimationParam& param){ pixelsCandleAnimationUpdate(param); });
  }
}

void Floower::stopAnimation(bool retainColor) {
  animations.StopAnimation(1);
  if (retainColor) {
    pixelsTargetColor = pixelsColor;
  }
}

void Floower::pixelsRainbowAnimationUpdate(const AnimationParam& param) {
  HsbColor hsbOriginal = HsbColor(pixelsOriginColor);
  float hue = hsbOriginal.H + param.progress;
  if (hue > 1.0) {
    hue = hue - 1;
  }
  pixelsColor = RgbColor(HsbColor(hue, 1, 0.4)); // TODO: fine tune
  showColor(pixelsColor);

  if (param.state == AnimationState_Completed) {
    if (pixelsTargetColor.CalculateBrightness() > 0) { // while there is something to show
      animations.RestartAnimation(param.index);
    }
  }
}

void Floower::pixelsCandleAnimationUpdate(const AnimationParam& param) {
  pixels.SetPixelColor(0, pixelsTargetColor);
  for (uint8_t i = 0; i < 6; i++) {
    pixels.SetPixelColor(i + 1, RgbColor::LinearBlend(candleOriginColors[i], candleTargetColors[i], param.progress));
  }

  if (param.state == AnimationState_Completed) {
    HsbColor candleHsbColor = HsbColor(candleColor);
    for (uint8_t i = 0; i < 6; i++) {
      candleHsbColor.B = random(20, 100) / 100.0;
      candleOriginColors[i] = candleTargetColors[i];
      candleTargetColors[i] = RgbColor(candleHsbColor);
    }
    animations.StartAnimation(param.index, random(10, 400), [=](const AnimationParam& param){ pixelsCandleAnimationUpdate(param); });
  }
}

void Floower::showColor(RgbColor color) {
  if (!lowPowerMode) {
    pixels.ClearTo(color);
  }
  else {
    pixels.ClearTo(colorBlack);
    pixels.SetPixelColor(0, color);
  }
}

bool Floower::isLit() {
  return pixelsPowerOn;
}

bool Floower::isAnimating() {
  return !animations.IsAnimating();
}

bool Floower::arePetalsMoving() {
  return animations.IsAnimationActive(0);
}

bool Floower::isChangingColor() {
  return (!interruptiblePixelsAnimation && animations.IsAnimationActive(1));
}

void Floower::acty() {
  digitalWrite(ACTY_LED_PIN, HIGH);
  actyOffTime = millis() + ACTY_BLINK_TIME;
}

bool Floower::setPixelsPowerOn(bool powerOn) {
  if (powerOn && !pixelsPowerOn) {
    pixelsPowerOn = true;
    ESP_LOGD(LOG_TAG, "LEDs power ON");
    digitalWrite(NEOPIXEL_PWR_PIN, LOW);
    delay(5); // TODO
    return true;
  }
  if (!powerOn && pixelsPowerOn) {
    pixelsPowerOn = false;
    ESP_LOGD(LOG_TAG, "LEDs power OFF");
    digitalWrite(NEOPIXEL_PWR_PIN, HIGH);
    return true;
  }
  return false; // no change
}

bool Floower::setServoPowerOn(bool powerOn) {
  if (powerOn && !servoPowerOn) {
    servoPowerOffTime = 0;
    servoPowerOn = true;
    ESP_LOGD(LOG_TAG, "Servo power ON");
    digitalWrite(SERVO_PWR_PIN, LOW);
    delay(5); // TODO
    return true;
  }
  if (!powerOn && servoPowerOn) {
    servoPowerOffTime = 0;
    servoPowerOn = false;
    ESP_LOGD(LOG_TAG, "Servo power OFF");
    digitalWrite(SERVO_PWR_PIN, HIGH);
    return true;
  }
  return false; // no change
}

Battery Floower::readBatteryState() {
  float reading = analogRead(BATTERY_ANALOG_PIN); // 0-4095
  float voltage = reading * 0.00181; // 1/4069 for scale * analog reference voltage is 3.6V * 2 for using 1:1 voltage divider + adjustment

  uint8_t level = _min(_max(0, voltage - 3.3) * 111, 100); // 3.3 .. 0%, 4.2 .. 100%

  ESP_LOGI(LOG_TAG, "Battery %.0f %.2fV %d%%", reading, voltage, level);

  batteryState = {voltage, level};
  return batteryState;
}

bool Floower::isUSBPowered() {
  if (config->hardwareRevision <= 5) {
    return true; // logic board with revision 5 lack the USB detection circuitry, pretend its always charging
  }
  float reading = analogRead(USB_ANALOG_PIN); // 0-4095
  return reading > 2000; // ~2900 is 5V
}

void Floower::setLowPowerMode(bool lowPowerMode) {
  if (this->lowPowerMode != lowPowerMode) {
    this->lowPowerMode = lowPowerMode;
    showColor(pixelsColor);
  }
}

bool Floower::isLowPowerMode() {
  return lowPowerMode;
}

void Floower::handleTimers(unsigned long now) {
  if (servoPowerOffTime > 0 && servoPowerOffTime < now) {
    servoPowerOffTime = 0;
    setServoPowerOn(false);
  }
  if (actyOffTime > 0 && actyOffTime < now) {
    actyOffTime = 0;
    digitalWrite(ACTY_LED_PIN, LOW); 
  }
}
