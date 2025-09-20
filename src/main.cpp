#include <Arduino.h>                  
#include <Wire.h>                    
#include <LiquidCrystal_I2C.h>        
#include <math.h>                   

LiquidCrystal_I2C lcd(0x27, 20, 4);   // LCD: адрес 0x27, 20 символов, 4 строки

#define POT_PIN         33             // Пин потенциометра (ADC вход ESP32)
#define START_BTN_PIN   19             // Пин кнопки старта заправки
#define SWITCH_BTN_PIN  18             // Пин кнопки переключения станций (экранов)

#define NUM_UNITS 1                    // Количество станций

// Кнстанты:
#define MIX_TANK_CAPACITY 20           // Объём бака смешивания, литров (фиксированный)
#define MS_PER_LITER      300          // Калибровка: миллисекунд на 1 литр (подстройкой добиваемся реального расхода)

// === Общий анод: инвертируем ШИМ (1=вкл) ===
// Если общий катод, ставим 0 (без инверсии).
#define COMMON_ANODE 0

// Структура одной станции (все пины и состояние)
struct Unit {
  // --- базовая логика ---
  int moisturePin;          // Пин датчика перелива бака дрона (PNP: HIGH=жидкость)
  int relayPin;             // Пин реле помпы №2 (перекачка из микс-бака в дрон)
  int targetLiters;         // Сколько литров нужно заправить в дрон
  int currentLiters;        // Текущее отображаемое значение (для обратного отсчёта на дисплее)
  int lastDisplayedLiters;  // Последнее показанное значение
  bool pumpRunning;         // Флаг: сейчас идёт перекачка в дрон
  bool readyToStart;        // Флаг: система готова начать новый цикл
  bool waitBeforeReset;     // Флаг: пауза перед новой заправкой (стабилизация)
  unsigned long waitStartTime;  // Время начала паузы
  unsigned long pumpStopTime;   // Время остановки текущей фазы
  bool needsDisplayUpdate;      // Принудительное обновление экрана

  // --- узел смешивания ---
  int mixMoisturePin;       // Пин датчика перелива микс-бака
  int pumpMixPin;           // Пин реле помпы №1 (наполнение микс-бака)
  int valveAPin;            // Пин клапана A (канал 95%)
  int valveBPin;            // Пин клапана B (канал 5%)

  bool fillingMix;          // Флаг: идёт наполнение микс-бака
  int deliveredLiters;      // Сколько уже перекачано в дрон суммарно
  int batchLiters;          // Объём текущей порции (<= MIX_TANK_CAPACITY)

  // --- строки дисплея ---
  String statusLine2;       // Запомненный текст 2-й строки LCD
  String statusLine3;       // Запомненный текст 3-й строки LCD

  // --- RGB индикация для станции ---
  int ledRPin, ledGPin, ledBPin;  // Пины каналов R/G/B
  int ledChR, ledChG, ledChB;     // Номера PWM-каналов LEDC (ESP32)
  float lastProgress01;           // Последний прогресс [0..1] (для установки цвета)
};

// Инициализация массива станций
Unit units[NUM_UNITS] = {
  // moisture relay tgt cur last pRun ready wait wStart stop upd  mixSens pumpMix valveA valveB  fillMix deliv batch s2  s3   R   G   B   chR chG chB lastP
  {   32,     26,   0,  0,  -1, false, true, false, 0,   0,  true,   34,    25,    27,    14,   false,   0,   0,  "",  "",  15,  2,  4,   -1, -1, -1, 0.0f }

};

int currentUnit = 0;                 // Индекс активной станции

// ---------------- LCD ----------------

// Функция печати строки с принудительной «очисткой» до конца строки
void lcdPrintClear(uint8_t col, uint8_t row, const String &text) {
  lcd.setCursor(col, row);          // Ставим курсор в нужную колонку/строку
  lcd.print("                    "); // 20 пробелов — очищаем строку целиком
  lcd.setCursor(col, row);          // Возвращаем курсор в начало строки
  lcd.print(text);                  // Печатаем текст
}

// Функция обновления строкт состояния и сохранения её значение
void updateStatusLine(Unit &u, uint8_t row, const String &text) {
  if (row == 2) u.statusLine2 = text;               //  текст 2-й строки
  if (row == 3) u.statusLine3 = text;               //  текст 3-й строки
  if (&u == &units[currentUnit]) {                  
    lcdPrintClear(0, row, text);                    
  }
}

// Полное «восстановление» экрана станции (при переключении окна)
void refreshDisplayForUnit(Unit &u) {
  lcd.clear();                                      // Очищаем весь экран
  lcd.setCursor(0, 0);                              // Заголовок
  lcd.print("station ");
  lcd.print(currentUnit + 1);
  lcdPrintClear(0, 1, "liters: " + String(u.targetLiters)); // Строка 1 — целевые литры
  if (u.statusLine2 != "") lcdPrintClear(0, 2, u.statusLine2); 
  if (u.statusLine3 != "") lcdPrintClear(0, 3, u.statusLine3); 
  u.needsDisplayUpdate = false;                    
}

// ---------------- RGB (ESP32 PWM) ----------------

// Для общего анода инверсия: яркость 0..255 → 255..0.
// Если COMMON_ANODE==0 — инверсия не нужна.
static inline int inv(int v){ return constrain(255 - constrain(v,0,255), 0, 255); }

// Установка RGB-яркости (0..255 на канал), с учётом инверсии для общего анода.
void ledWriteRGB(const Unit &u, int r, int g, int b) {
#if COMMON_ANODE
  ledcWrite(u.ledChR, inv(r));      // Пишем инвертированное значение на канал R
  ledcWrite(u.ledChG, inv(g));      // Пишем инвертированное значение на канал G
  ledcWrite(u.ledChB, inv(b));      // Пишем инвертированное значение на канал B
#else
  ledcWrite(u.ledChR, constrain(r, 0, 255));  // Без инверсии (общий катод)
  ledcWrite(u.ledChG, constrain(g, 0, 255));
  ledcWrite(u.ledChB, constrain(b, 0, 255));
#endif
}

// Быстрое выключение RGB
void ledOff(const Unit &u) { ledWriteRGB(u, 0, 0, 0); }

// Мгновенный «красный» (для аварии)
void ledRed(const Unit &u) { ledWriteRGB(u, 255, 0, 0); }

// Простой градиент
static inline float lerp(float a, float b, float t){ return a + (b - a) * t; }

// Генерация цвета по прогрессу p в [0..1]:
// 0.00 → Синий, 0.33 → Зелёный, 0.66 → Оранжевый, 1.00 → Красный.
void colorFromProgress(float p, int &r, int &g, int &b) {
  p = constrain(p, 0.0f, 1.0f);           // Нормируем
  if (p < 0.3333f) {                      // Участок: синий → зелёный
    float t = p / 0.3333f;
    r = 0;
    g = (int)lerp(0, 255, t);
    b = (int)lerp(255, 0, t);
  } else if (p < 0.6666f) {               // Участок: зелёный → оранжевый
    float t = (p - 0.3333f) / 0.3333f;
    r = (int)lerp(0, 255, t);
    g = (int)lerp(255, 128, t);
    b = 0;
  } else {                                // Участок: оранжевый → красный
    float t = (p - 0.6666f) / 0.3334f;
    r = 255;
    g = (int)lerp(128, 0, t);
    b = 0;
  }
}

void ledUpdateGradient(Unit &u, float progress01) {
  u.lastProgress01 = constrain(progress01, 0.0f, 1.0f);
  int r,g,b; colorFromProgress(u.lastProgress01, r, g, b);
  ledWriteRGB(u, r, g, b);                              
}

// ---------------- buttons ----------------

// Обработка переключения активной станции
void handleUnitSwitch() {
  static bool lastState = HIGH;                         
  bool currentState = digitalRead(SWITCH_BTN_PIN);     
  if (lastState == HIGH && currentState == LOW) {      
    currentUnit = (currentUnit + 1) % NUM_UNITS;        // Перейти к следующей станции по кругу
    refreshDisplayForUnit(units[currentUnit]);          // Восстановить экран активной станции
  }
  lastState = currentState;                             // Обновить «предыдущее» состояние
}

// Реле активны LOW: LOW=включено, HIGH=выключено
inline void pumpMixOn(const Unit& u)   { digitalWrite(u.pumpMixPin, LOW);  } // Включить помпу №1
inline void pumpMixOff(const Unit& u)  { digitalWrite(u.pumpMixPin, HIGH); } // Выключить помпу №1
inline void pumpDroneOn(const Unit& u) { digitalWrite(u.relayPin, LOW);    } // Включить помпу №2
inline void pumpDroneOff(const Unit& u){ digitalWrite(u.relayPin, HIGH);   } // Выключить помпу №2
inline void valvesOpen(const Unit& u)  { digitalWrite(u.valveAPin, LOW); digitalWrite(u.valveBPin, LOW); } // Открыть оба клапана
inline void valvesClose(const Unit& u) { digitalWrite(u.valveAPin, HIGH); digitalWrite(u.valveBPin, HIGH);} // Закрыть оба клапана

// ----------- Процесс: фаза заполнения микс-бака -----------
void startFillingMix(Unit &u) {
  u.fillingMix = true;                                               // Фазу A
  u.pumpRunning = false;                                             // Помпа №2 не должна работать
  u.batchLiters = min(MIX_TANK_CAPACITY, u.targetLiters - u.deliveredLiters); // Порция: не больше 20л и не больше остатка
  if (u.batchLiters <= 0) {                                          // Нечего качать — цикл завершён
    u.waitBeforeReset = true;                                        // Ставим паузу перед «готово»
    u.waitStartTime = millis();                                      // Начало паузы
    updateStatusLine(u, 2, "ready again");                           // Выводимстатус
    return;                                                 
  }
  valvesOpen(u);                                                     // Открываем клапаны A и B
  pumpMixOn(u);                                                      // Включаем помпу №1
  u.pumpStopTime = millis() + (unsigned long)(u.batchLiters * MS_PER_LITER); // Срок окончания фазы A по времени

  updateStatusLine(u, 2, "mix <- " + String(u.batchLiters));         // На 2-й строке показываем стартовый объём

  // RGB: в фазе mix лишь показывает текущмй прогресс (цвет не переливается)
  if (u.targetLiters > 0) {
    float p = (float)u.deliveredLiters / (float)u.targetLiters;      // Прогресс по уже доставленным литрам
    ledUpdateGradient(u, p);                                         // Установить цвет
  }
}

// Остановка фазы наполнения микс-бака
void stopFillingMix(Unit &u) {
  pumpMixOff(u);                                                     // Выключаем помпу №1
  valvesClose(u);                                                    // Закрываем клапаны
  u.fillingMix = false;                                       
}

// ----------- Процесс: фаза перекачки в дрон -----------
void startPumpingDrone(Unit &u) {
  u.pumpRunning = true;                                              // Фаза B активна
  pumpDroneOn(u);                                                    // Включаем помпу №2
  u.pumpStopTime = millis() + (unsigned long)(u.batchLiters * MS_PER_LITER); // Срок окончания фазы B по времени

  updateStatusLine(u, 2, "pump on <- " + String(u.batchLiters));     // На 2-й строке показываем стартовый объём

  // RGB: установить цвет исходя из уже доставленного объёма
  if (u.targetLiters > 0) {
    float p = (float)u.deliveredLiters / (float)u.targetLiters;
    ledUpdateGradient(u, p);
  }
}

// Остановка фазы перекачки в дрон
void stopPumpingDrone(Unit &u) {
  pumpDroneOff(u);                                                   // Выключаем помпу №2
  u.pumpRunning = false;                               
}

// ----------- Инициализацияпинов станции -----------
void setupUnitIO(Unit &u, int idx) {
  pinMode(u.moisturePin, INPUT);                                     // Датчик перелива дрона
  pinMode(u.relayPin, OUTPUT);                                       // Реле помпы №2
  pinMode(u.mixMoisturePin, INPUT);                                  // Датчик перелива микс-бака
  pinMode(u.pumpMixPin, OUTPUT);                                     // Реле помпы №1
  pinMode(u.valveAPin, OUTPUT);                                      // Реле клапана A
  pinMode(u.valveBPin, OUTPUT);                                      // Реле клапана B

  pumpDroneOff(u);                                                   // Все исполнительные — в OFF
  pumpMixOff(u);
  valvesClose(u);

  u.pumpRunning = false;                                             // Стартовые флаги/значения
  u.readyToStart = true;
  u.waitBeforeReset = false;
  u.needsDisplayUpdate = true;
  u.lastDisplayedLiters = -1;
  u.pumpStopTime = 0;
  u.targetLiters = 0;
  u.currentLiters = 0;

  u.fillingMix = false;
  u.deliveredLiters = 0;
  u.batchLiters = 0;

  u.statusLine2 = "";                                                // Сброс кэша строк
  u.statusLine3 = "";

  // Выделяем 3 PWM-канала под станцию (R/G/B)
  u.ledChR = idx*3 + 0;
  u.ledChG = idx*3 + 1;
  u.ledChB = idx*3 + 2;

  ledcSetup(u.ledChR, 5000, 8);                                      // Частота 5 кГц, 8 бит (0..255)
  ledcSetup(u.ledChG, 5000, 8);
  ledcSetup(u.ledChB, 5000, 8);

  ledcAttachPin(u.ledRPin, u.ledChR);                                // Привязываем пины к каналам
  ledcAttachPin(u.ledGPin, u.ledChG);
  ledcAttachPin(u.ledBPin, u.ledChB);

  ledOff(u);                                                         // На старте — свет выключен
  u.lastProgress01 = 0.0f;                                           // Прогресс памяти = 0
}

// ----------- глобальная инициализация -----------
void setup() {
  Serial.begin(115200);                                              // UART для отладки
  Wire.begin(21, 22);                                                // I2C с явными SDA=21, SCL=22 (ESP32)
  lcd.init();                                                        // Инициализация LCD
  lcd.backlight();                                                   // Подсветка LCD
  lcd.setCursor(0, 0);
  lcd.print("system on");                                            // Сообщение о работе системы
  delay(500);                                                   

  pinMode(POT_PIN, INPUT);                                           // Потенциометр — вход (ADC)
  pinMode(START_BTN_PIN, INPUT_PULLUP);                              // Кнопка старта
  pinMode(SWITCH_BTN_PIN, INPUT_PULLUP);                             // Кнопка переключения

  for (int i = 0; i < NUM_UNITS; i++) {                              // Инициализация всех станций
    setupUnitIO(units[i], i);                                        // Индекс для назначения PWM-каналов
  }

  lcd.clear();                                                       // Начальный экран
  lcd.setCursor(0, 0);
  lcd.print("station 1");
  lcdPrintClear(0, 1, "liters: 0");                                  // Строка 1: литры=0 (до вращения потенциометра)
}

// ----------- главный цикл -----------
void loop() {
  handleUnitSwitch();                                                // Обработка переключения станций
  Unit &unit = units[currentUnit];                                   // текущая активная станция

  if (unit.needsDisplayUpdate) {                                     // Если требуется принудительное обновление экрана
    refreshDisplayForUnit(unit);                                   
  }

  // --- Режим ожидания ---
  if (!unit.fillingMix && !unit.pumpRunning && unit.readyToStart) {
    int potValue = analogRead(POT_PIN);                              // Считываем потенциометр (0..4095)
    unit.targetLiters = map(potValue, 0, 4095, 1, 100);              // Переводим в диапазон 1..100 литров
    if (unit.targetLiters != unit.lastDisplayedLiters || unit.needsDisplayUpdate) {
      unit.currentLiters = unit.targetLiters;                        // Для согласованности отображения
      lcdPrintClear(0, 1, "liters: " + String(unit.targetLiters));   // Показать целевое
      unit.lastDisplayedLiters = unit.targetLiters;                  
      unit.needsDisplayUpdate = false;                               
    }
  }

  // --- Старт цикла по кнопке ---
  if (!unit.fillingMix && !unit.pumpRunning && unit.readyToStart && digitalRead(START_BTN_PIN) == LOW) {
    delay(50);                                                       
    if (digitalRead(START_BTN_PIN) == LOW) {                         
      unit.readyToStart = false;                                     
      unit.deliveredLiters = 0;                                      
      unit.needsDisplayUpdate = true;                                
      startFillingMix(unit);                                         
      while (digitalRead(START_BTN_PIN) == LOW);                     
    }
  }

  // --- Фаза A: наполнение микс-бака ---
  if (unit.fillingMix) {
    bool mixOverflow = digitalRead(unit.mixMoisturePin) == HIGH;     // Контроль перелива микс-бака
    int remainingL = max((int)((unit.pumpStopTime - millis()) / MS_PER_LITER), 0); // Оставшееся «время»
    if (remainingL != unit.currentLiters) {                          // Обновлять строку только при изменении
      unit.currentLiters = remainingL;
      updateStatusLine(unit, 2, "mix <- " + String(unit.currentLiters)); // Отсчёт для фазы A
    }

    if (millis() >= unit.pumpStopTime || mixOverflow) {              // Условия завершения фазы A
      if (mixOverflow) unit.batchLiters = MIX_TANK_CAPACITY;         // Если сработал датчик — бак полный
      stopFillingMix(unit);                                          // Остановить помпу №1 и закрыть клапаны
      startPumpingDrone(unit);                                       // Перейти к фазе B
    }
  }

  // --- Фаза B: перекачка в дрон ---
  if (unit.pumpRunning) {
    bool droneOverflow = digitalRead(unit.moisturePin) == HIGH;      // Контроль перелива бака дрона
    int remainingL = max((int)((unit.pumpStopTime - millis()) / MS_PER_LITER), 0); // Оставшееся «время»
    if (remainingL != unit.currentLiters) {                          // Обновление строки статуса
      unit.currentLiters = remainingL;
      updateStatusLine(unit, 2, "pump on <- " + String(unit.currentLiters)); // Отсчёт для фазы B
    }

    // RGB-индикация
    if (unit.targetLiters > 0) {
      int pumpedThisBatch = unit.batchLiters - remainingL;           // Сколько литров перелилось в этой партии
      if (pumpedThisBatch < 0) pumpedThisBatch = 0;                 
      float progress01 = (float)(unit.deliveredLiters + pumpedThisBatch) / (float)unit.targetLiters; // 0..1
      ledUpdateGradient(unit, progress01);                           // Обновить цвет по прогрессу
    }

    if (millis() >= unit.pumpStopTime || droneOverflow) {            // Условия завершения фазы B
      stopPumpingDrone(unit);                                        // Остановить помпу №2

      if (droneOverflow) {                                           // Авария по датчику дрона
        ledRed(unit);                                                // Мгновенно красный
        unit.deliveredLiters = unit.targetLiters;                    // Считаем цель достигнутой (останавливаем цикл)
        updateStatusLine(unit, 2, "filled in ");                     // Сообщение о переливе
        unit.waitBeforeReset = true;                                 
        unit.waitStartTime = millis();
      } else {                                                       // Нормальное окончание заправки по времени
        unit.deliveredLiters += unit.batchLiters;                    // Добавляем в прогресс
        if (unit.deliveredLiters >= unit.targetLiters) {             // При завершении
          unit.deliveredLiters = unit.targetLiters;                  
          updateStatusLine(unit, 2, "pump off ");                    // Сообщение «помпа выкл»
          ledOff(unit);                                              // Готово — выключаем диод
          unit.waitBeforeReset = true;                               
          unit.waitStartTime = millis();
        } else {                                                     // Иначе — повторить
          startFillingMix(unit);                                     // Вернуться к фазе A
        }
      }
    }
  }

  // --- Пауза перед разрешением нового запуска ---
  if (unit.waitBeforeReset) {
    if (millis() - unit.waitStartTime >= 1000) {                     
      unit.readyToStart = true;                                      // Разрешить новый цикл
      unit.waitBeforeReset = false;                                  // Снять флаг паузы
      unit.needsDisplayUpdate = true;                              
      unit.fillingMix = false;                                       // Для нормализации снимаем флаги фаз
      unit.pumpRunning = false;
      updateStatusLine(unit, 2, "ready again");                      // Сообщение о повторной подготовке к работе
      ledOff(unit);                                                  // Выключаем диод (в т.ч. после аварии)
    }
  }

  delay(50);                                                       
}