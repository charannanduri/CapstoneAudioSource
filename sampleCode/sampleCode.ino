#include <Arduino.h>
#include "driver/i2s.h"
#include "FS.h"       
#include "SPIFFS.h"   

// I2S Pin Config
#define I2S_BCLK_PIN     (GPIO_NUM_6)  // D6
#define I2S_LRCK_PIN     (GPIO_NUM_2)  // D10
#define I2S_DOUT_PIN     (GPIO_NUM_7)  // D7
#define I2S_DIN_PIN      (I2S_PIN_NO_CHANGE) // Not used

//Audio Config
#define I2S_PORT_NUM     (I2S_NUM_0) // Use I2S Port 0
// Buffer to hold audio data read from file before sending to I2S
// Increase size for potentially smoother playback, decrease if memory is tight
#define I2S_BUFFER_SIZE  (1024 * 2) // Bytes. Must be multiple of sample size (e.g., 4 for 16-bit stereo)
uint8_t i2s_write_buffer[I2S_BUFFER_SIZE];

//WAV File Config
#define AUDIO_FILENAME   "/audio.wav" 

//Variables
File audioFile;
// WAV Header Info
uint32_t sampleRate = 0;
uint16_t bitsPerSample = 0;
uint16_t numChannels = 0;
uint32_t dataStartPos = 0;

//Function Prototypes 
bool setupI2S(uint32_t rate, uint16_t bits, uint16_t channels);
bool readWavHeader(File file);
void playWavChunk();

void setup() {
  Serial.begin(115200);
  Serial.println("\nESP32-C3 I2S WAV Player");

  //Initialize SPI filesystem
  Serial.print("Initialize SPIFFS");
  if (!SPIFFS.begin(true)) { // Format SPIFFS if mount failed
    Serial.println("SPIFFS Mount Failed. Formatting...");
     if(!SPIFFS.begin(true)){ // Try again after format
         Serial.println("SPIFFS Mount failed even after formatting!");
         while(1) delay(1000); // Halt
     }
  }
  Serial.println("OK");

  // Open the WAV file
  Serial.printf("Opening file: %s\n", AUDIO_FILENAME);
  audioFile = SPIFFS.open(AUDIO_FILENAME, FILE_READ);
  if (!audioFile) {
    Serial.println("Failed to open file");
    Serial.println("use 'ESP32 Sketch Data Upload'?");
    while (1) delay(1000); // Halt
  }
  Serial.printf("File size: %lu bytes\n", audioFile.size());

  //Read WAV Header and get properties
  Serial.print("Reading WAV header... ");
  if (!readWavHeader(audioFile)) {
    Serial.println("Failed to read WAV header.");
    audioFile.close();
    while (1) delay(1000); // Halt
  }
  Serial.printf("OK (Rate: %d, Bits: %d, Channels: %d, Data Start: %d)\n",
                sampleRate, bitsPerSample, numChannels, dataStartPos);

  // Check supported format (Ex: only 16-bit)
   if (bitsPerSample != 16) {
      Serial.printf("Unsupported bits per sample: %d \n", bitsPerSample);
      audioFile.close();
      while(1) delay(1000); // Halt
   }
   if (numChannels < 1 || numChannels > 2) {
       Serial.printf("Unsupported channel count: %d (Only 1 or 2 supported)\n", numChannels);
       audioFile.close();
       while(1) delay(1000); // Halt
   }


  //Configure I2S based on WAV header
  Serial.print("Configuring I2S... ");
  if (!setupI2S(sampleRate, bitsPerSample, numChannels)) {
     Serial.println("Failed to configure I2S.");
     audioFile.close();
     while(1) delay(1000); // Halt
  }
   Serial.println("OK");

  // put file pointer at the start of audio
  audioFile.seek(dataStartPos);

  Serial.println("Setup complete, playing...");
}

void loop() {
  // Continuously read chunks and send to I2S
  playWavChunk();
}

//Helper Functions ---

/**
 * @brief Configures and installs the I2S driver based on WAV parameters.
 * @param rate Sample rate (Hz)
 * @param bits Bits per sample (e.g., 16)
 * @param channels Number of channels (1 or 2)
 * @return true on success, false on failure
 */
bool setupI2S(uint32_t rate, uint16_t bits, uint16_t channels) {
  // Validate parameters slightly
  if (bits != 16 || (channels != 1 && channels != 2)) {
      Serial.printf("I2S Setup Error: Unsupported format (Bits: %d, Channels: %d)\n", bits, channels);
      return false;
  }

  // I2S Configuration
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = rate,
    .bits_per_sample = (i2s_bits_per_sample_t)bits,
    // Note: ESP-IDF I2S driver expects stereo data even for mono files when using I2S_CHANNEL_FMT_RIGHT_LEFT.
    // We will handle mono-to-stereo conversion in playWavChunk if needed.
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8, // More buffers can help prevent underruns
    .dma_buf_len = I2S_BUFFER_SIZE / 8, // Should be multiple of 4 bytes (sample frame size)
    .use_apll = false, // Use internal APLL clock - set false for C3/S3 for stability sometimes
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  // I2S Pin Configuration
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_LRCK_PIN,
    .data_out_num = I2S_DOUT_PIN,
    .data_in_num = I2S_DIN_PIN
  };

  // Install and start I2S driver
  esp_err_t err = i2s_driver_install(I2S_PORT_NUM, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("Failed to install I2S driver: %s\n", esp_err_to_name(err));
    return false;
  }

  err = i2s_set_pin(I2S_PORT_NUM, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("Failed to set I2S pins: %s\n", esp_err_to_name(err));
    i2s_driver_uninstall(I2S_PORT_NUM); // Clean up driver install
    return false;
  }

  // i2s_zero_dma_buffer(I2S_PORT_NUM);

  return true;
}

/**
 * @brief Reads the WAV file header to extract audio format information.
 * Assumes a standard RIFF WAV format. More robust parsing might be needed for unusual files.
 * @param file The opened WAV file object.
 * @return true if header is valid and info extracted, false otherwise.
 */
bool readWavHeader(File file) {
  if (!file || file.size() < 44) { // Basic check for size
    return false;
  }

  // Read the header into a buffer
  uint8_t header[44];
  if (file.read(header, 44) != 44) {
    return false;
  }

  //Validate
  // Check RIFF chunk descriptor
  if (header[0] != 'R' || header[1] != 'I' || header[2] != 'F' || header[3] != 'F') {
    Serial.println("Error: Not a RIFF file");
    return false;
  }
  // Check WAVE format
  if (header[8] != 'W' || header[9] != 'A' || header[10] != 'V' || header[11] != 'E') {
    Serial.println("Error: Not a WAVE file");
    return false;
  }
  // Check subchunk
  if (header[12] != 'f' || header[13] != 'm' || header[14] != 't' || header[15] != ' ') {
    Serial.println("Error: 'fmt ' chunk not found");
    return false;
  }
   // Check audio format (1 = PCM)
  uint16_t audioFormat = header[20] | (header[21] << 8);
  if (audioFormat != 1) {
      Serial.printf("Error: Unsupported audio format: %d (only PCM=1 supported)\n", audioFormat);
      return false;
  }

  //Extract Information (Little-Endian assumed)
  numChannels = header[22] | (header[23] << 8);
  sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
  bitsPerSample = header[34] | (header[35] << 8);

  // Find 'data' chunk ---
  // The 'data' chunk might not start exactly at byte 36 if there are extra fmt chunks.
  // We'll search for it starting from byte 12 (after 'WAVE').
  uint32_t searchPos = 12;
  bool dataChunkFound = false;
  while (searchPos < file.size() - 8) {
      file.seek(searchPos);
      if (file.read(header, 8) != 8) break; // Read chunk ID and size

      if (header[0] == 'd' && header[1] == 'a' && header[2] == 't' && header[3] == 'a') {
          dataStartPos = searchPos + 8; // Data starts after ID and size fields
          uint32_t dataSize = header[4] | (header[5] << 8) | (header[6] << 16) | (header[7] << 24);
          Serial.printf(" (Found 'data' chunk at %d, size %d)", searchPos, dataSize);
          dataChunkFound = true;
          break;
      }
      // If not 'data', skip this chunk: ID (4 bytes) + Size (4 bytes) + chunk data
      uint32_t chunkSize = header[4] | (header[5] << 8) | (header[6] << 16) | (header[7] << 24);
      searchPos += 8 + chunkSize;
      // Add padding byte if chunk size is odd
      if (chunkSize % 2 != 0) {
          searchPos++;
      }
  }

  if (!dataChunkFound) {
      Serial.println("Error: 'data' chunk not found in file.");
      return false;
  }

  // Reset file position for reading later if needed (though setup() will seek again)
  file.seek(dataStartPos);
  return true;
}


/**
 * @brief Reads a chunk from the WAV file and writes it to the I2S peripheral.
 * Handles mono-to-stereo conversion if necessary and file looping.
 */
void playWavChunk() {
  if (!audioFile) {
    return; // File not open
  }

  size_t bytes_to_read = 0;
  size_t bytes_to_write_i2s = 0;

  // Calculate how much to read based on buffer size and file format
  if (numChannels == 1 && bitsPerSample == 16) {
      // Mono: Read half the buffer, as we will duplicate samples for stereo output
      bytes_to_read = I2S_BUFFER_SIZE / 2;
      bytes_to_write_i2s = I2S_BUFFER_SIZE; // We will write a full stereo buffer
  } else if (numChannels == 2 && bitsPerSample == 16) {
      // Stereo: Read the full buffer size
      bytes_to_read = I2S_BUFFER_SIZE;
      bytes_to_write_i2s = I2S_BUFFER_SIZE;
  } else {
      Serial.println("playWavChunk: Unsupported format!"); // Should have been caught earlier
      return;
  }

  // Check if we need to loop
  if (audioFile.available() < bytes_to_read) {
      if (audioFile.available() == 0) { // Reached exact end
          Serial.println("End of file reached. Looping...");
          audioFile.seek(dataStartPos); // Go back to the start of audio data
      } else {
          // Read the remaining partial data first if any
          bytes_to_read = audioFile.available();
           if (numChannels == 1) bytes_to_write_i2s = bytes_to_read * 2;
           else bytes_to_write_i2s = bytes_to_read;
           // Ensure bytes_to_write_i2s is a multiple of the frame size (4 bytes for 16-bit stereo)
           bytes_to_write_i2s = (bytes_to_write_i2s / 4) * 4;
           if (numChannels == 1) bytes_to_read = bytes_to_write_i2s / 2;
           else bytes_to_read = bytes_to_write_i2s;

           if (bytes_to_read == 0) { // If remaining bytes < frame size, just loop
               Serial.println("End of file reached (partial frame skipped). Looping...");
               audioFile.seek(dataStartPos);
               bytes_to_read = (numChannels == 1) ? I2S_BUFFER_SIZE / 2 : I2S_BUFFER_SIZE;
               bytes_to_write_i2s = I2S_BUFFER_SIZE;
           }
      }
  }


  // Read data from file
  size_t bytes_read = audioFile.read(i2s_write_buffer, bytes_to_read);

  if (bytes_read > 0) {
      size_t bytes_written_to_i2s = 0;

      if (numChannels == 1 && bitsPerSample == 16) {
          // Mono to Stereo conversion: Duplicate samples
          // We read `bytes_read` mono samples into the first half of the buffer.
          // Now expand it into the full buffer as LRLR...
          int16_t* samples_mono = (int16_t*)i2s_write_buffer;
          int16_t* samples_stereo = (int16_t*)i2s_write_buffer; // Overwrite in place from end
          size_t mono_samples_count = bytes_read / sizeof(int16_t);

          for (int i = mono_samples_count - 1; i >= 0; --i) {
              samples_stereo[2 * i + 1] = samples_mono[i]; // Right channel
              samples_stereo[2 * i] = samples_mono[i];     // Left channel
          }
          bytes_to_write_i2s = bytes_read * 2; // We now have twice the data (stereo)
      } else {
          // Stereo: Data is already in the correct format (LRLR...)
          bytes_to_write_i2s = bytes_read;
      }

      // Write data to I2S DAC
      esp_err_t result = i2s_write(I2S_PORT_NUM, i2s_write_buffer, bytes_to_write_i2s, &bytes_written_to_i2s, portMAX_DELAY);

      if (result != ESP_OK) {
          Serial.printf("I2S Write Error: %s\n", esp_err_to_name(result));
      }
      if (bytes_written_to_i2s < bytes_to_write_i2s) {
          Serial.printf("I2S Underrun Warning: Wrote only %d / %d bytes\n", bytes_written_to_i2s, bytes_to_write_i2s);
          // Potential buffer size issue or CPU too slow
      }
  } else if (bytes_read == 0 && audioFile.available() == 0) {
      // End of file reached during read, loop now
      Serial.println("End of file reached during read. Looping...");
      audioFile.seek(dataStartPos);
  } else if (bytes_read < 0) {
      Serial.println("Error reading from file.");
      // Consider stopping playback or trying again
  }
}
