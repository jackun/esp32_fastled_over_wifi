// Enable PSRAM from Arduino IDE

#include <esp_psram.h>

//#define FASTLED_ESP32_I2S
//#define FASTLED_USES_ESP32S3_I2S

#include "FastLED.h"
#include "WifiSetup.h"
#include "AsyncUDP.h"
#include <NetworkClient.h>
#include <WebServer.h>
#include <uri/UriBraces.h>

#define MY_BUILTIN_LED 21 //GPIO21
#define LED_PIN 5
#define NUM_LEDS 128

CRGB leds[NUM_LEDS];
uint8_t * ledsRaw = (uint8_t *)leds;
uint8_t current_led_num = NUM_LEDS;
uint8_t demo_reel = 0, brightness = 32;

// A 'magic word' (along with LED count & checksum) precedes each block
// of LED data; this assists the microcontroller in syncing up with the
// host-side software and properly issuing the latch (host I/O is
// likely buffered, making usleep() unreliable for latch).  You may see
// an initial glitchy frame or two until the two come into alignment.
// The magic word can be whatever sequence you like, but each character
// should be unique, and frequent pixel values like 0 and 255 are
// avoided -- fewer false positives.  The host software will need to
// generate a compatible header: immediately following the magic word
// are three bytes: a 16-bit count of the number of LEDs (high byte
// first) followed by a simple checksum value (high byte XOR low byte
// XOR 0x55).  LED data follows, 3 bytes per LED, in order R, G, B,
// where 0 = off and 255 = max brightness.

static const uint8_t magic[] = {
  'A','d','a'};
#define MAGICSIZE  sizeof(magic)
#define HEADERSIZE (MAGICSIZE + 3)

#define MODE_HEADER 0
#define MODE_DATA   2

WebServer server(80);
AsyncUDP udp;


/* demoreel start */
#define FRAMES_PER_SECOND  120
// List of patterns to cycle through.  Each is defined as a separate function below.
typedef void (*SimplePatternList[])();

uint8_t gCurrentPatternNumber = 0; // Index number of which pattern is current
uint8_t gHue = 0; // rotating "base color" used by many of the patterns

#define ARRAY_SIZE(A) (sizeof(A) / sizeof((A)[0]))

void rainbow() 
{
  // FastLED's built-in rainbow generator
  fill_rainbow( leds, NUM_LEDS, gHue, 7);
}

void rainbowWithGlitter() 
{
  // built-in FastLED rainbow, plus some random sparkly glitter
  rainbow();
  addGlitter(80);
}

void addGlitter( fract8 chanceOfGlitter) 
{
  if( random8() < chanceOfGlitter) {
    leds[ random16(NUM_LEDS) ] += CRGB::White;
  }
}

void confetti() 
{
  // random colored speckles that blink in and fade smoothly
  fadeToBlackBy( leds, NUM_LEDS, 10);
  int pos = random16(NUM_LEDS);
  leds[pos] += CHSV( gHue + random8(64), 200, 255);
}

void sinelon()
{
  // a colored dot sweeping back and forth, with fading trails
  fadeToBlackBy( leds, NUM_LEDS, 20);
  int pos = beatsin16( 13, 0, NUM_LEDS-1 );
  leds[pos] += CHSV( gHue, 255, 192);
}

void bpm()
{
  // colored stripes pulsing at a defined Beats-Per-Minute (BPM)
  uint8_t BeatsPerMinute = 62;
  CRGBPalette16 palette = PartyColors_p;
  uint8_t beat = beatsin8( BeatsPerMinute, 64, 255);
  for( int i = 0; i < NUM_LEDS; i++) { //9948
    leds[i] = ColorFromPalette(palette, gHue+(i*2), beat-gHue+(i*10));
  }
}

void juggle() {
  // eight colored dots, weaving in and out of sync with each other
  fadeToBlackBy( leds, NUM_LEDS, 20);
  byte dothue = 0;
  for( int i = 0; i < 8; i++) {
    leds[beatsin16( i+7, 0, NUM_LEDS-1 )] |= CHSV(dothue, 200, 255);
    dothue += 32;
  }
}

SimplePatternList gPatterns = { rainbow, rainbowWithGlitter, confetti, sinelon, juggle, bpm };

void nextPattern()
{
  // add one to the current pattern number, and wrap around at the end
  gCurrentPatternNumber = (gCurrentPatternNumber + 1) % ARRAY_SIZE( gPatterns);
}

/* demoreel end */

void reset_leds() {
  Serial.println("Clear leds");
  FastLED.clear(true);
  Serial.printf("Add leds %d\n", current_led_num);
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, /*NUM_LEDS*/ current_led_num);
  Serial.println("memset leds");
  memset(leds, 0,  NUM_LEDS * sizeof(struct CRGB)); //filling Led array by zeroes
  Serial.println("brightness");
  FastLED.setBrightness(brightness);
  Serial.println("Reset leds done");
}

void handleRoot() {
  //digitalWrite(led, 1);
  server.send(200, "text/plain", "hello from esp32!\n");
  //digitalWrite(led, 0);
}

void handleNotFound() {
  //digitalWrite(led, 1);
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
  //digitalWrite(led, 0);
}

File rawFile;
void handleCreate() {
  server.send(200, "text/plain", "");
}

uint8_t mode = MODE_HEADER;
int32_t bytesRemaining;
unsigned long
  startTime,
  lastByteTime,
  lastAckTime,
  t;
int32_t outPos = 0;

void handleLEDPacket(uint8_t *buffer, int16_t bytesBuffered) {
  uint8_t i,
  hi, lo, chk,
  indexIn       = 0,
  indexOut      = 0;

  //if (FastLED.size() != current_led_num) {
  //  reset_leds();
  //}
  while (bytesBuffered> 0) {
    Serial.printf("bytesRemaining: %d, buffered: %d\n", bytesRemaining, bytesBuffered);
    if (!bytesRemaining && bytesBuffered < 3 * current_led_num + HEADERSIZE) {
      startTime  = micros();
      mode       = MODE_HEADER; // Begin next header search
      FastLED.show();
      break;
    }
    switch(mode) {
    case MODE_HEADER:
      // In header-seeking mode.  Is there enough data to check?
      if(bytesBuffered >= HEADERSIZE) {
        // Indeed.  Check for a 'magic word' match.
        for(i=0; (i<MAGICSIZE) && (buffer[indexOut++] == magic[i++]););
        if(i == MAGICSIZE) {
          Serial.println("Found header");
          // Magic word matches.  Now how about the checksum?
          hi  = buffer[indexOut++];
          lo  = buffer[indexOut++];
          chk = buffer[indexOut++];
          if(chk == (hi ^ lo ^ 0x55)) {
            Serial.println("Check ok");
            // Checksum looks valid.  Get 16-bit LED count, add 1
            // (# LEDs is always > 0) and multiply by 3 for R,G,B.
            bytesRemaining = 3L * (256L * (long)hi + (long)lo + 1L);
            bytesBuffered -= 3;
            outPos = 0;
            memset(leds, 0,  NUM_LEDS * sizeof(struct CRGB));
            mode           = MODE_DATA; // Proceed to latch wait mode
          } 
          else {
            // Checksum didn't match; search resumes after magic word.
            indexOut  -= 3; // Rewind
          }
        } // else no header match.  Resume at first mismatched byte.
        bytesBuffered -= i;
      }
      break;

    case MODE_DATA:
      if(bytesRemaining > 0) {
        if(bytesBuffered > 0) {
          if (outPos < sizeof(leds))
            ledsRaw[outPos++] = buffer[indexOut++];   // Issue next byte
          bytesBuffered--;
          bytesRemaining--;
        }
        // If serial buffer is threatening to underrun, start
        // introducing progressively longer pauses to allow more
        // data to arrive (up to a point).
      } 
      else {
        // End of data -- issue latch:
        startTime  = micros();
        mode       = MODE_HEADER; // Begin next header search
        FastLED.show();
      }
    } // end switch
  }
}

void handleCreateProcess() {
  HTTPRaw &raw = server.raw();
  if (raw.status == RAW_START) {
    startTime = micros();
    mode      = MODE_HEADER; // Begin next header search
    Serial.println("Upload: START");
  } else if (raw.status == RAW_WRITE) {
    handleLEDPacket(raw.buf, raw.currentSize);
    Serial.print("Upload: WRITE, Bytes: ");
    Serial.println(raw.currentSize);
  } else if (raw.status == RAW_END) {
    FastLED.show();
    Serial.print("Upload: END, Size: ");
    Serial.println(raw.totalSize);
  }
}

void handleMode() {
  String arg0 = server.pathArg(0);
  String arg1 = server.pathArg(1);
  if (arg0 == "demo") {
    if (arg1 == "1")
      demo_reel = 1;
    else if (arg1 == "0")
      demo_reel = 2;
    else if (arg1 == "next")
      nextPattern();
  }

  if (arg0 == "leds") {
    current_led_num = std::clamp(int(arg1.toInt()), 1, NUM_LEDS);
  }

  if (arg0 == "bright") {
    brightness = std::clamp(int(arg1.toInt()), 1, 255);
  }
  server.send(200, "text/plain", "");
}

void TaskBlink(void *pvParameters) {
  for(;;) {
    server.handleClient();
  }
}

void udpPacket(AsyncUDPPacket packet) {
  /*
  Serial.print("UDP Packet Type: ");
  Serial.print(packet.isBroadcast() ? "Broadcast" : packet.isMulticast() ? "Multicast" : "Unicast");
  Serial.print(", From: ");
  Serial.print(packet.remoteIP());
  Serial.print(":");
  Serial.print(packet.remotePort());
  Serial.print(", To: ");
  Serial.print(packet.localIP());
  Serial.print(":");
  Serial.print(packet.localPort());
  Serial.print(", Length: ");
  Serial.print(packet.length());
  Serial.print(", Data: ");
  Serial.write(packet.data(), packet.length());
  Serial.println();
  */

  if (packet.length() > 1) {
    uint8_t *data = packet.data();
    //Serial.printf("Type: %d\n", data[0]);
    //Serial.printf("Wait: %d\n", data[1]);
    if (data[0] == 2) {
      demo_reel = 0;
      int len = min(packet.length(), 3 * (size_t)current_led_num);
      while (len > 0) {
        ledsRaw[len] = data[2 + len];
        len--;
      }
      FastLED.show();
    }
  }

  //handleLEDPacket(packet.data(), packet.length());
  //reply to the client
  //packet.printf("Got %u bytes of data", packet.length());
}

void setup() {
  psramInit(); // IMPORTANT: This is required to enable PSRAM. If you don't do this, the driver will not work.

  rgbLedWrite(MY_BUILTIN_LED, 0, 64, 0); // OK led

  Serial.begin(115200);
  //while(!Serial);

  // This is used so that you can see if PSRAM is enabled. If not, we will crash in setup() or in loop().
  log_d("Total heap: %d", ESP.getHeapSize());
  log_d("Free heap: %d", ESP.getFreeHeap());
  log_d("Total PSRAM: %d", ESP.getPsramSize());  // If this prints out 0, then PSRAM is not enabled.
  log_d("Free PSRAM: %d", ESP.getFreePsram());

  //log_d("waiting 6 second before startup");
  //delay(6000);  // The long reset time here is to make it easier to flash the device during the development process.

  reset_leds();
  log_d("Show leds");
  // Call the current pattern function once, updating the 'leds' array
  gPatterns[gCurrentPatternNumber]();
  FastLED.show();
  FastLED.delay(1000);

  wifi_connect();
  wifi_update();

  server.on("/", handleRoot);
  server.on(UriBraces("/mode/{}/{}"), handleMode);
  server.on("/leds", HTTP_PUT, handleCreate, handleCreateProcess);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");

  if (udp.listen(1234)) {
    udp.onPacket(udpPacket);
  }

  log_d("Stack: %d\n", uxTaskGetStackHighWaterMark(NULL));

  xTaskCreate(
    TaskBlink, "Task Blink"  // A name just for humans
    ,
    2048 * 2  // The stack size can be checked by calling `uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);`
    ,
    nullptr  // Task parameter which can modify the task behavior. This must be passed as pointer to void.
    ,
    2  // Priority
    ,
    NULL  // Task handle is not used here - simply pass NULL
  );

  uint8_t rled = 0, bled = 0;
  for (;;){

    if (demo_reel == 1)
    {
      if (FastLED.size() != current_led_num) {
        Serial.printf("Current led num: %d\n", FastLED.size());
        reset_leds();
      }
      // Call the current pattern function once, updating the 'leds' array
      gPatterns[gCurrentPatternNumber]();
      // send the 'leds' array out to the actual LED strip
      FastLED.show();  
      // insert a delay to keep the framerate modest
      FastLED.delay(1000/FRAMES_PER_SECOND); 
      // do some periodic updates
      EVERY_N_MILLISECONDS( 200 ) { gHue++; } // slowly cycle the "base color" through the rainbow
      EVERY_N_SECONDS( 10 ) { nextPattern(); } // change patterns periodically
    }
    if (demo_reel == 2)
    {
      demo_reel = 0;
      memset(leds, 0,  NUM_LEDS * sizeof(struct CRGB)); //filling Led array by zeroes
      FastLED.show();
    }


    EVERY_N_SECONDS(1){
      bled = rled;
      rled = rled ? 0 : 32;
      rgbLedWrite(MY_BUILTIN_LED, rled, 0, bled);
    };
    EVERY_N_SECONDS(15) {
      wifi_update();
    };
  }
}

void loop() {
  //server.handleClient();
  //delay(2);  //allow the cpu to switch to other tasks
}

