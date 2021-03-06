#include <SPI.h>
#include <SdFat.h>
#include <FAB_LED.h>

#define DEBUG

// ports used:
// digital 4 - SD card chip select
// digital 6 - strip of 200 APA106 LEDs
// digital 11 - SPI (SD card DI)
// digital 12 - SPI (SD card DO)
// digital 13 - SPI (SD card CLK)
// analog 0 - RF remote button D
// analog 1 - RF remote button C
// analog 2 - RF remote button B
// analog 3 - RF remote button A

enum class Pressed : uint8_t {
  none = 0,
  a    = 8,
  b    = 4,
  c    = 2,
  d    = 1,
  abcd = a|b|c|d,
  abc  = a|b|c,
  abd  = a|b|d,
  ab   = a|b,
  acd  = a|c|d,
  ac   = a|c,
  ad   = a|d,
  bcd  = b|c|d,
  bc   = b|c,
  bd   = b|d,
  cd   = c|d,
};

enum class Mode : uint8_t {
  NORMAL,
  COLOR_CEILING,
  COLOR_FLOOR,
  VIDEO_PLAYBACK,
  MACRO,
  MISC,
  ROOT_DIR_ON_RELEASE,
} CurrentMode;

struct PendingOperations
{
  uint8_t cycle_brightness;
  uint8_t next_video;
  uint8_t prev_video;

  uint8_t cycle_ceiling_red;
  uint8_t cycle_ceiling_green;
  uint8_t cycle_ceiling_blue;

  uint8_t cycle_floor_red;
  uint8_t cycle_floor_green;
  uint8_t cycle_floor_blue;

  uint8_t reduce_speed;
  uint8_t increase_speed;
  uint8_t toggle_pause;

  uint8_t toggle_negative;
  uint8_t reset_settings;

  uint8_t toggle_frame_len;
};

volatile PendingOperations GlobalPendingOperations;

apa106<D, 6>   LEDstrip;
rgb frame[200];
uint8_t brightness = 255;
uint8_t negative;

struct ColorIntensity
{
  enum { MIN = 0, MAX = 8 };

  uint8_t floor = MIN;
  uint8_t ceil = MAX;

  void lowerCeil()
  {
    ceil = (ceil == floor ? MAX : ceil - 1);
  }

  void raiseFloor()
  {
    floor = (ceil == floor ? MIN : floor + 1);
  }

  void reset()
  {
    floor = MIN;
    ceil = MAX;
  }

  uint8_t minBrightness()
  {
    return map(floor, MIN, MAX, negative * brightness, !negative * brightness);
  }

  uint8_t maxBrightness()
  {
    return map(ceil, MIN, MAX, negative * brightness, !negative * brightness);
  }
};

ColorIntensity r_intensity;
ColorIntensity g_intensity;
ColorIntensity b_intensity;
#define BRIGHTNESS_STEP 32

int8_t speed;
const int8_t MIN_SPEED = -6;     // Playing backward at 5x normal rate
const int8_t MAX_SPEED = 4;      // Playing forward at 5x normal rate

int8_t frame_len;
const int8_t MIN_FRAME_LEN = -1; // Each frame lasts 0.5x normal time (25ms shorter)
const int8_t MAX_FRAME_LEN = 4;  // Each frame lasts 3x normal time (100ms longer)

//------------------------------------------------------------------------------
// File system object.
SdFat sd;

FatFile root_dir;
File root_file;

FatFile rainbows_dir;
File rainbows_file;

FatFile *curr_dir = &root_dir;
File *infile = &root_file;

unsigned long startMillis;

// Serial streams
ArduinoOutStream cout(Serial);

// SD card chip select
const int chipSelect = 4;

void setup()
{
  pinMode(A0, INPUT_PULLUP);
  pinMode(A1, INPUT_PULLUP);
  pinMode(A2, INPUT_PULLUP);
  pinMode(A3, INPUT_PULLUP);

  drawSplashScreen(frame);
  LEDstrip.sendPixels(sizeof(frame) / sizeof(*frame), frame);

  Serial.begin(9600);

  // Wait for USB Serial
  while (!Serial) {
    SysCall::yield();
  }

  cout << F("\nInitializing SD.\n");
  if (!sd.begin(chipSelect, SPI_FULL_SPEED)) {
    if (sd.card()->errorCode()) {
      cout << F("SD initialization failed.\n");
      cout << F("errorCode: ") << hex << showbase;
      cout << int(sd.card()->errorCode());
      cout << F(", errorData: ") << int(sd.card()->errorData());
      cout << dec << noshowbase << endl;
      return;
    }

    cout << F("\nCard successfully initialized.\n");
    if (sd.vol()->fatType() == 0) {
      cout << F("Can't find a valid FAT16/FAT32 partition.\n");
      return;
    }
    if (!sd.vwd()->isOpen()) {
      cout << F("Can't open root directory.\n");
      return;
    }
    cout << F("Can't determine error type\n");
    return;
  }
  cout << F("\nCard successfully initialized.\n");
  cout << endl;

  sd.ls();

  root_dir.openRoot(&sd);
  rainbows_dir.open(&root_dir, "rainbows", O_READ);
  Serial.println(F("rainbows:"));
  rainbows_dir.ls();

  nextFile();
  startMillis = millis();

  PCMSK1 = B1111; // Enable interrupts for inputs A0, A1, A2, A3
  PCIFR  |= bit(PCIF1);   // clear any outstanding interrupts
  PCICR  |= bit(PCIE1);   // enable pin change interrupts for D8 to D13
}

void printPendingOperations(PendingOperations & ops)
{
  if (ops.cycle_brightness)    Serial.print(F("\ncycle_brightness"));
  if (ops.next_video)          Serial.print(F("\nnext_video"));
  if (ops.prev_video)          Serial.print(F("\nprev_video"));
  if (ops.cycle_ceiling_red)   Serial.print(F("\ncycle_ceiling_red"));
  if (ops.cycle_ceiling_green) Serial.print(F("\ncycle_ceiling_green"));
  if (ops.cycle_ceiling_blue)  Serial.print(F("\ncycle_ceiling_blue"));
  if (ops.cycle_floor_red)     Serial.print(F("\ncycle_floor_red"));
  if (ops.cycle_floor_green)   Serial.print(F("\ncycle_floor_green"));
  if (ops.cycle_floor_blue)    Serial.print(F("\ncycle_floor_blue"));
  if (ops.reduce_speed)        Serial.print(F("\nreduce_speed"));
  if (ops.increase_speed)      Serial.print(F("\nincrease_speed"));
  if (ops.toggle_pause)        Serial.print(F("\ntoggle_pause"));
  if (ops.toggle_negative)     Serial.print(F("\ntoggle_negative"));
  if (ops.reset_settings)      Serial.print(F("\nreset_settings"));
  if (ops.toggle_frame_len)    Serial.print(F("\ntoggle_frame_len"));
}

void printSettings()
{
  Serial.print(F("\nBrightness: max "));
  Serial.print(brightness);
  Serial.print(F(" r "));
  Serial.print(r_intensity.floor);
  Serial.print(F("-"));
  Serial.print(r_intensity.ceil);
  Serial.print(F(" g "));
  Serial.print(g_intensity.floor);
  Serial.print(F("-"));
  Serial.print(g_intensity.ceil);
  Serial.print(F(" b "));
  Serial.print(b_intensity.floor);
  Serial.print(F("-"));
  Serial.print(b_intensity.ceil);
  Serial.print(F(" neg "));
  Serial.print(negative);
  Serial.print(F(" speed "));
  Serial.print(speed);
  Serial.print(F(" frame_len "));
  Serial.print(frame_len);
}

void resetDefaultSettings()
{
  r_intensity.reset();
  g_intensity.reset();
  b_intensity.reset();
  brightness = 255;
  negative = 0;
  speed = 0;
  frame_len = 0;
}

void performPendingOperations(PendingOperations & ops)
{
#ifdef DEBUG
  printPendingOperations(ops);
#endif

  if (ops.cycle_brightness)    brightness -= BRIGHTNESS_STEP;
  if (ops.next_video)          nextFile();
  if (ops.prev_video)          previousFile();
  if (ops.cycle_ceiling_red)   r_intensity.lowerCeil();
  if (ops.cycle_ceiling_green) g_intensity.lowerCeil();
  if (ops.cycle_ceiling_blue)  b_intensity.lowerCeil();
  if (ops.cycle_floor_red)     r_intensity.raiseFloor();
  if (ops.cycle_floor_green)   g_intensity.raiseFloor();
  if (ops.cycle_floor_blue)    b_intensity.raiseFloor();
  if (ops.reduce_speed)        speed = max(speed-1, MIN_SPEED);
  if (ops.increase_speed)      speed = min(speed+1, MAX_SPEED);
  if (ops.toggle_pause)        speed = (speed == -1 ? 0 : -1);
  if (ops.toggle_negative)     negative = !negative;
  if (ops.reset_settings)      resetDefaultSettings();
  if (ops.toggle_frame_len)    frame_len = (frame_len == MAX_FRAME_LEN ? MIN_FRAME_LEN : frame_len+1);

#ifdef DEBUG
  if (    ops.cycle_brightness
       || ops.next_video
       || ops.prev_video
       || ops.cycle_ceiling_red
       || ops.cycle_ceiling_green
       || ops.cycle_ceiling_blue
       || ops.cycle_floor_red
       || ops.cycle_floor_green
       || ops.cycle_floor_blue
       || ops.reduce_speed
       || ops.increase_speed
       || ops.toggle_pause
       || ops.toggle_negative
       || ops.reset_settings
       || ops.toggle_frame_len)
  {
    printSettings();
  }
#endif
}

ISR(PCINT1_vect) // handle pin change interrupt for A0 to A5
{
  // Atomically read pins A0-A5, mask off A4 and A5, and represent
  // the states of A0, A1, A2, and A3 as a Pressed enum object
  auto button_states = static_cast<Pressed>(PINC & B1111);

  switch(CurrentMode) {
    case Mode::NORMAL: {
      switch (button_states) {
        case Pressed::a: {
          GlobalPendingOperations.cycle_brightness = 1;
        } break;

        case Pressed::b: {
          GlobalPendingOperations.next_video = 1;
        } break;

        case Pressed::ab: {
          CurrentMode = Mode::COLOR_CEILING;
        } break;

        case Pressed::cd: {
          CurrentMode = Mode::COLOR_FLOOR;
        } break;

        case Pressed::ad: {
          CurrentMode = Mode::VIDEO_PLAYBACK;
        } break;

        case Pressed::c: {
          CurrentMode = Mode::MACRO;
        } break;

        case Pressed::bc: {
          CurrentMode = Mode::MISC;
        } break;
      }
    } break;

    case Mode::COLOR_CEILING: {
      switch (button_states) {
        case Pressed::d: {
          CurrentMode = Mode::NORMAL;
        } break;

        case Pressed::a: {
          GlobalPendingOperations.cycle_ceiling_red = 1;
        } break;

        case Pressed::b: {
          GlobalPendingOperations.cycle_ceiling_green = 1;
        } break;

        case Pressed::c: {
          GlobalPendingOperations.cycle_ceiling_blue = 1;
        } break;

      }
    } break;

    case Mode::COLOR_FLOOR: {
      switch (button_states) {
        case Pressed::d: {
          CurrentMode = Mode::NORMAL;
        } break;

        case Pressed::a: {
          GlobalPendingOperations.cycle_floor_red = 1;
        } break;

        case Pressed::b: {
          GlobalPendingOperations.cycle_floor_green = 1;
        } break;

        case Pressed::c: {
          GlobalPendingOperations.cycle_floor_blue = 1;
        } break;
      }
    } break;

    case Mode::VIDEO_PLAYBACK: {
      switch (button_states) {
        case Pressed::d: {
          CurrentMode = Mode::NORMAL;
        } break;

        case Pressed::a: {
          GlobalPendingOperations.reduce_speed = 1;
        } break;

        case Pressed::b: {
          GlobalPendingOperations.increase_speed = 1;
        } break;

        case Pressed::c: {
          GlobalPendingOperations.toggle_pause = 1;
        } break;

        case Pressed::ac: {
          GlobalPendingOperations.prev_video = 1;
        } break;

        case Pressed::bd: {
          GlobalPendingOperations.next_video = 1;
        } break;
      }
    } break;

    case Mode::MISC: {
      switch (button_states) {
        case Pressed::d: {
          CurrentMode = Mode::NORMAL;
        } break;

        case Pressed::a: {
          GlobalPendingOperations.toggle_negative = 1;
        } break;

        case Pressed::b: {
          GlobalPendingOperations.reset_settings = 1;
        } break;
      }
    } break;

    case Mode::MACRO: {
      if (button_states == Pressed::none) {
        break;
      }

      static Pressed MacroButtons[3];
      static int8_t MacroButtonCount = 0;

      MacroButtons[MacroButtonCount++] = button_states;
      if ((MacroButtonCount %= 3) != 0) {
        break;
      }

      CurrentMode = Mode::NORMAL;

      switch (MacroButtons[0]) {
        case Pressed::c: {
          switch (MacroButtons[1]) {
            case Pressed::a: {
              switch (MacroButtons[2]) {
                case Pressed::a: {
                  // caa - permanent switch to root directory
                  if (root_dir.isOpen()) {
                    curr_dir = &root_dir;
                    infile = &root_file;
                  }
                } break;

                case Pressed::b: {
                  // cab - permanent switch to rainbows directory
                  if (rainbows_dir.isOpen()) {
                    curr_dir = &rainbows_dir;
                    infile = &rainbows_file;
                  }
                } break;
              }
            } break;

            case Pressed::c: {
              switch (MacroButtons[2]) {
                case Pressed::c: {
                  // ccc - toggle frame len
                  GlobalPendingOperations.toggle_frame_len = 1;
                } break;
              }
            } break;
          }
        } break;

        case Pressed::d: {
          switch (MacroButtons[1]) {
            case Pressed::a: {
              switch (MacroButtons[2]) {
                case Pressed::b: {
                  // dab - temporary switch to rainbows directory
                  if (rainbows_dir.isOpen()) {
                    curr_dir = &rainbows_dir;
                    infile = &rainbows_file;
                    CurrentMode = Mode::ROOT_DIR_ON_RELEASE;
                  }
                } break;
              }
            } break;
          }
        } break;
      }
    } break;

    case Mode::ROOT_DIR_ON_RELEASE: {
      if (button_states == Pressed::none) {
        curr_dir = &root_dir;
        infile = &root_file;
        CurrentMode = Mode::NORMAL;
      }
    } break;
  }
}

void nextFile()
{
  if (infile->isOpen()) {
    infile->close();
  }

  while (true) {
    infile->openNext(curr_dir);
    if (infile->isOpen()) {
      if (!infile->isDir() && !infile->isHidden() && !infile->isSystem()) {
#ifdef DEBUG
        char buf[13];
        infile->getName(buf, sizeof(buf));
        cout << F("\nOpened file: ") << buf;
#endif
        return;
      }
      infile->close();
    } else {
      curr_dir->rewind();
    }
  }
}

void previousFile()
{
  if (infile->isOpen()) {
    infile->close();
  }

  while (true) {
    // dir size is 32.
    uint16_t index = curr_dir->curPosition()/32;
    if (index < 2) {
      // Advance to past last file of directory.
      dir_t dir;
      while (curr_dir->readDir(&dir) > 0);
      continue;
    }
    // position to possible previous file location.
    index -= 2;

    do {
      infile->open(curr_dir, index, O_READ);

      if (infile->isOpen()) {
        if (!infile->isDir() && !infile->isHidden() && !infile->isSystem()) {
#ifdef DEBUG
          char buf[13];
          infile->getName(buf, sizeof(buf));
          cout << F("\nOpened prev file: ") << buf;
#endif
          return;
        }
        infile->close();
      }
    } while (index-- > 0);
  }
}

bool readFrame()
{
  int32_t seek_offset = (int32_t)speed * sizeof(frame);
  if (speed < 0 && infile->curPosition() < -seek_offset) {
    // Going backwards, and reached the start of this file.
    previousFile();
    // Successfully opened previous file - start from the end.
    infile->seekEnd();
  }
  if (!infile->seekCur(seek_offset)) {
    return false;
  }
  return infile->read(frame, sizeof(frame)) == sizeof(frame);
}

void adjustFrameColors()
{
  const uint8_t r_min_brightness = r_intensity.minBrightness();
  const uint8_t g_min_brightness = g_intensity.minBrightness();
  const uint8_t b_min_brightness = b_intensity.minBrightness();

  const uint8_t r_max_brightness = r_intensity.maxBrightness();
  const uint8_t g_max_brightness = g_intensity.maxBrightness();
  const uint8_t b_max_brightness = b_intensity.maxBrightness();

  for (auto & f : frame) {
    f.r = map(f.r, 0, 255, r_min_brightness, r_max_brightness);
    f.g = map(f.g, 0, 255, g_min_brightness, g_max_brightness);
    f.b = map(f.b, 0, 255, b_min_brightness, b_max_brightness);
  }
}

void loop()
{
  if (!readFrame()) {
    nextFile();
    startMillis = millis();
    return;
  }

  PendingOperations pending_operations;
  noInterrupts();
  memcpy(&pending_operations, &GlobalPendingOperations, sizeof(pending_operations));
  memset(&GlobalPendingOperations, 0, sizeof(GlobalPendingOperations));
  interrupts();

  performPendingOperations(pending_operations);

  adjustFrameColors();

  unsigned long frame_time = 50 + frame_len * 25UL;
  while (millis() - startMillis < frame_time) {
    // busy loop until its time to paint the lights
  }
  startMillis += frame_time;

  LEDstrip.sendPixels(sizeof(frame) / sizeof(*frame), frame);
}
