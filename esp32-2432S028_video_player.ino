// Tutorial : https://youtu.be/jYcxUgxz9ks
// Use board "ESP32 Dev Module" (last tested on v3.2.0)

#include <Arduino_GFX_Library.h> // Install "GFX Library for Arduino" with the Library Manager (last tested on v1.6.0)
                                 // Install "JPEGDEC" with the Library Manager (last tested on v1.8.2)
#include "MjpegClass.h"          // Included in this project
#include "SD.h"                  // Included with the Espressif Arduino Core (last tested on v3.2.0)
#include <TFT_Touch.h>           // Touch screen library - Install "TFT_Touch" with the Library Manager

// Touch screen pins (XPT2046 on ESP32-2432S028)
#define T_DO   39   // Data out pin (MISO)
#define T_DIN  32   // Data in pin (MOSI)
#define T_CS   33   // Chip select pin
#define T_CLK  25   // Clock pin

// Pins for the display
#define BL_PIN 21 // On some cheap yellow display model, BL pin is 27
#define SD_CS 5
#define SD_MISO 19
#define SD_MOSI 23
#define SD_SCK 18

#define BOOT_PIN 0                   // Boot pin
#define BOOT_BUTTON_DEBOUCE_TIME 400 // Debounce time when reading the boot button in milliseconds
#define TOUCH_POLL_INTERVAL_MS 100   // Poll touch screen every 100ms instead of every frame

// Some model of cheap Yellow display works only at 40Mhz
// #define DISPLAY_SPI_SPEED 40000000L // 40MHz
#define DISPLAY_SPI_SPEED 80000000L // 80MHz


#define SD_SPI_SPEED 80000000L      // 80Mhz

const char *MJPEG_FOLDER = "/mjpeg"; // Name of the mjpeg folder on the SD Card

// Storage for files to read on the SD card
#define MAX_FILES 20 // Maximum number of files, adjust as needed
String mjpegFileList[MAX_FILES];
uint32_t mjpegFileSizes[MAX_FILES] = {0}; // Store each GIF file's size in bytes
int mjpegCount = 0;
static int currentMjpegIndex = 0;
static File mjpegFile; // temp gif file holder

// Global variables for mjpeg
MjpegClass mjpeg;
int total_frames;
unsigned long total_read_video;
unsigned long total_decode_video;
unsigned long total_show_video;
unsigned long start_ms, curr_ms;
long output_buf_size, estimateBufferSize;
uint8_t *mjpeg_buf;
uint16_t *output_buf;

// Display global variables
Arduino_DataBus *bus = new Arduino_HWSPI(2 /* DC */, 15 /* CS */, 14 /* SCK */, 13 /* MOSI */, 12 /* MISO */);
Arduino_GFX *gfx = new Arduino_ILI9341(bus);

// SD Card reader is on a separate SPI
SPIClass sd_spi(VSPI);

// Touch screen instance
TFT_Touch touch = TFT_Touch(T_CS, T_CLK, T_DIN, T_DO);

// Interrupt to skip to the next mjpeg when the boot button is pressed
volatile bool skipRequested = false; // set in ISR, read in loop()
volatile uint32_t isrTick = 0;       // tick count captured in ISR
uint32_t lastPress = 0;              // used in main context for debounc

void IRAM_ATTR onButtonPress()
{
    skipRequested = true;                 // flag handled in the playback loop
    isrTick = xTaskGetTickCountFromISR(); // safe, 1-tick resolution
}

void setup()
{
    Serial.begin(115200);

    // Set display backlight to High
    pinMode(BL_PIN, OUTPUT);
    digitalWrite(BL_PIN, HIGH);

    // Display initialization
    Serial.println("Display initialization");
    if (!gfx->begin(DISPLAY_SPI_SPEED))
    {
        Serial.println("Display initialization failed!");
        while (true)
        {
            /* no need to continue */
        }
    }
    gfx->setRotation(0);
    gfx->fillScreen(RGB565_BLACK);
    // gfx->invertDisplay(true); // on some cheap yellow models, display must be inverted
    Serial.printf("Screeen size Width=%d,Height=%d\n", gfx->width(), gfx->height());

    // Touch screen initialization
    touch.setRotation(0); // Match display rotation

    // SD card initialization
    Serial.println("SD Card initialization");
    if (!SD.begin(SD_CS, sd_spi, SD_SPI_SPEED, "/sd"))
    {
        Serial.println("ERROR: File system mount failed!");
        while (true)
        {
            /* no need to continue */
        }
    }

    // Buffer allocation for mjpeg playing
    Serial.println("Buffer allocation");
    output_buf_size = gfx->width() * 16 * 2;  // Increased from 4 to 16 scanlines
    output_buf = (uint16_t *)heap_caps_aligned_alloc(16, output_buf_size * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!output_buf)
    {
        Serial.println("output_buf aligned_alloc failed!");
        while (true)
        {
            /* no need to continue */
        }
    }
    estimateBufferSize = gfx->width() * gfx->height() * 3 / 5;  // Increased from 2/5 to 3/5 (60% of screen)
    mjpeg_buf = (uint8_t *)heap_caps_malloc(estimateBufferSize, MALLOC_CAP_8BIT);
    if (!mjpeg_buf)
    {
        Serial.println("mjpeg_buf allocation failed!");
        while (true)
        {
            /* no need to continue */
        }
    }

    loadMjpegFilesList(); // Load the list of mjpeg to play from the SD card

    // Set the boot button to skip the current mjpeg playing and go to the next
    pinMode(BOOT_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(BOOT_PIN), // fast ISR
                    onButtonPress, FALLING);         // press == LOW
}

void loop()
{
    playSelectedMjpeg(currentMjpegIndex);
    currentMjpegIndex++;
    if (currentMjpegIndex >= mjpegCount)
    {
        currentMjpegIndex = 0;
    }
}

// Play the current mjpeg
void playSelectedMjpeg(int mjpegIndex)
{
    // Build the full path for the selected mjpeg
    String fullPath = String(MJPEG_FOLDER) + "/" + mjpegFileList[mjpegIndex];
    char mjpegFilename[128];
    fullPath.toCharArray(mjpegFilename, sizeof(mjpegFilename));

    Serial.printf("Playing %s\n", mjpegFilename);
    mjpegPlayFromSDCard(mjpegFilename);
}

// Callback function to draw a JPEG
int jpegDrawCallback(JPEGDRAW *pDraw)
{
    unsigned long s = millis();
    gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
    total_show_video += millis() - s;
    return 1;
}

// Play a mjpeg stored on the SD card
void mjpegPlayFromSDCard(char *mjpegFilename)
{
    Serial.printf("Opening %s\n", mjpegFilename);
    File mjpegFile = SD.open(mjpegFilename, "r");

    if (!mjpegFile || mjpegFile.isDirectory())
    {
        Serial.printf("ERROR: Failed to open %s file for reading\n", mjpegFilename);
    }
    else
    {
        Serial.println("MJPEG start");
        gfx->fillScreen(RGB565_BLACK);

        start_ms = millis();
        curr_ms = millis();
        total_frames = 0;
        total_read_video = 0;
        total_decode_video = 0;
        total_show_video = 0;

        mjpeg.setup(
            &mjpegFile, mjpeg_buf, jpegDrawCallback, true /* useBigEndian */,
            0 /* x */, 0 /* y */, gfx->width() /* widthLimit */, gfx->height() /* heightLimit */);

        unsigned long lastTouchPoll = 0;
        while (!skipRequested && mjpegFile.available() && mjpeg.readMjpegBuf())
        {
            // Check for touch input to skip video (polled less frequently to save CPU)
            unsigned long now = millis();
            if (now - lastTouchPoll >= TOUCH_POLL_INTERVAL_MS)
            {
                lastTouchPoll = now;
                if (touch.Pressed())
                {
                    skipRequested = true;
                }
            }

            // Read video
            total_read_video += millis() - curr_ms;
            curr_ms = millis();

            // Play video
            mjpeg.drawJpg();
            total_decode_video += millis() - curr_ms;

            curr_ms = millis();
            total_frames++;
        }
        /* We exited because the button was pressed or the video ended */
        if (skipRequested) // pressed?
        {
            uint32_t now = millis(); // safe here
            if (now - lastPress < BOOT_BUTTON_DEBOUCE_TIME)
            {
                // ignore if it was within the debounce time
            }
            else
            {
                lastPress = now;
            }
        }
        skipRequested = false;

        int time_used = millis() - start_ms;
        Serial.println(F("MJPEG end"));
        mjpegFile.close();
        skipRequested = false; // ready for next video
        float fps = 1000.0 * total_frames / time_used;
        total_decode_video -= total_show_video;
        Serial.printf("Total frames: %d\n", total_frames);
        Serial.printf("Time used: %d ms\n", time_used);
        Serial.printf("Average FPS: %0.1f\n", fps);
        Serial.printf("Read MJPEG: %lu ms (%0.1f %%)\n", total_read_video, 100.0 * total_read_video / time_used);
        Serial.printf("Decode video: %lu ms (%0.1f %%)\n", total_decode_video, 100.0 * total_decode_video / time_used);
        Serial.printf("Show video: %lu ms (%0.1f %%)\n", total_show_video, 100.0 * total_show_video / time_used);
        Serial.printf("Video size (wxh): %d×%d, scale factor=%d\n",mjpeg.getWidth(),mjpeg.getHeight(),mjpeg.getScale());
    }
}

// Read the mjpeg file list in the mjpeg folder of the SD card
void loadMjpegFilesList()
{
    File mjpegDir = SD.open(MJPEG_FOLDER);
    if (!mjpegDir)
    {
        Serial.printf("Failed to open %s folder\n", MJPEG_FOLDER);
        while (true)
        {
            /* code */
        }
    }
    mjpegCount = 0;
    while (true)
    {
        File file = mjpegDir.openNextFile();
        if (!file)
            break;
        if (!file.isDirectory())
        {
            String name = file.name();
            if (name.endsWith(".mjpeg"))
            {
                mjpegFileList[mjpegCount] = name;
                mjpegFileSizes[mjpegCount] = file.size(); // Save file size (in bytes)
                mjpegCount++;
                if (mjpegCount >= MAX_FILES)
                    break;
            }
        }
        file.close();
    }
    mjpegDir.close();
    Serial.printf("%d mjpeg files read\n", mjpegCount);
    // Optionally, print out each file's size for debugging:
    for (int i = 0; i < mjpegCount; i++)
    {
        Serial.printf("File %d: %s, Size: %lu bytes (%s)\n", i, mjpegFileList[i].c_str(), mjpegFileSizes[i],formatBytes(mjpegFileSizes[i]).c_str());
    }
}

// Function helper display sizes on the serial monitor
String formatBytes(size_t bytes)
{
    if (bytes < 1024)
    {
        return String(bytes) + " B";
    }
    else if (bytes < (1024 * 1024))
    {
        return String(bytes / 1024.0, 2) + " KB";
    }
    else
    {
        return String(bytes / 1024.0 / 1024.0, 2) + " MB";
    }
}
