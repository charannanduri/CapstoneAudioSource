import time
import board
import audiocore
import audiobusio
import digitalio

# --- I2S Setup ---
lck_pin = board.D5   # Word Select
bck_pin = board.D4   # Bit Clock
dat_pin = board.D6    # Data

audio = audiobusio.I2SOut(bit_clock=bck_pin, word_select=lck_pin, data=dat_pin)

# --- LED Setup ---
led = digitalio.DigitalInOut(board.D1)
led.direction = digitalio.Direction.OUTPUT
led.value = False

led2 = digitalio.DigitalInOut(board.D2)
led2.direction = digitalio.Direction.OUTPUT
led2.value = True   # Always on

# --- Button Setup ---
button = digitalio.DigitalInOut(board.D0)
button.direction = digitalio.Direction.INPUT
button.pull = digitalio.Pull.UP

# --- Audio File Setup ---
wav = audiocore.WaveFile(open("audio.wav", "rb"))

# --- State Variables ---
playing = False
last_button_val = True  # Tracks the previous state of the button
last_led_time = 0       # Tracks the last time the LED toggled
LED_INTERVAL = 0.3      # Blink speed (0.3 seconds)

while True:
    # 1. Capture the current time
    now = time.monotonic()
    
    # 2. Read button state
    current_button_val = button.value

    # 3. Detect "Edge": Did button go from High (released) to Low (pressed)?
    if current_button_val is False and last_button_val is True:
        # A tiny sleep just for debounce is okay (20ms is imperceptible)
        time.sleep(0.02)
        if button.value is False:
            if playing:
                audio.stop()
                playing = False
                led.value = False
            else:
                # Use loop=True (lowercase 'l') to loop the audio
                audio.play(wav, loop=True)
                playing = True
    
    # Save current button state for the next loop comparison
    last_button_val = current_button_val

    # 4. Handle LED Blinking (Non-Blocking)
    if playing:
        # Check if enough time has passed since the last toggle
        if now - last_led_time >= LED_INTERVAL:
            led.value = not led.value
            last_led_time = now
    
    # No time.sleep() here! The loop runs as fast as possible to catch inputs.
