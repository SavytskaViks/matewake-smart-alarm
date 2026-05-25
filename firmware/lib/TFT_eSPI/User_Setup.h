

#define USER_SETUP_LOADED

#define ST7789_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// 3. Піни підключення (перевірте фізичне підключення)
#define TFT_MISO 19  // Master Input Slave Output
#define TFT_MOSI 23  // Master Output Slave Input
#define TFT_SCLK 18  // Serial Clock
#define TFT_CS   5  // Chip Select (активний низький)
#define TFT_DC    4  // Data/Command (низький = команда)
#define TFT_RST   16  // Reset (або підключити до RESET Arduino)

// 4. Оптимізовано частоту SPI (для Uno/Nano рекомендується до 20 МГц)
#define SPI_FREQUENCY  10000000    // Стабільна робота на Uno/Nano

// 5. Шрифти (видаліть невикористовувані для економії пам'яті)
#define LOAD_GLCD   // Стандартний шрифт 5x7
//#define LOAD_FONT2  // Шрифт 8x16 (потрібно розкоментувати)
//#define LOAD_FONT4  // Шрифт 16x26
//#define LOAD_FONT6  // Шрифт 24x40
//#define LOAD_FONT7  // Шрифт 32x53
//#define LOAD_FONT8  // Шрифт 40x67
#define LOAD_GFXFF  // FreeFonts (плавкі шрифти)

// 6. Плавкі шрифти (потрібен LOAD_GFXFF)
#define SMOOTH_FONT

// 7. Вимкнути SPI для сенсора (якщо не використовується)
// #define SPI_TOUCH_FREQUENCY 2500000

// 8. Додати підтримку транзакцій для стабільності
#define SUPPORT_TRANSACTIONS

// 9. Додаткова оптимізація (видалити для плат з малою пам'яттю)
#define ESP32_DMA  // Для ESP32 
#define TFT_RGB_ORDER TFT_BGR  // Корекція кольорів