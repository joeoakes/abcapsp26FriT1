from evdev import InputDevice, ecodes

dev = InputDevice('/dev/input/event9')
print("Listening on", dev.path)

for event in dev.read_loop():
    if event.type == ecodes.EV_KEY and event.value == 1:
        if event.code == ecodes.KEY_UP:
            print("UP")
        elif event.code == ecodes.KEY_DOWN:
            print("DOWN")
        elif event.code == ecodes.KEY_LEFT:
            print("LEFT")
        elif event.code == ecodes.KEY_RIGHT:
            print("RIGHT")
        elif event.code == ecodes.KEY_ENTER:
            print("ENTER")
        elif event.code == ecodes.KEY_X:
            print("X")
        elif event.code == ecodes.KEY_Z:
            print("Z")
        elif event.code == ecodes.KEY_S:
            print("S")
        elif event.code == ecodes.KEY_M:
            print("M")
        elif event.code == ecodes.KEY_RIGHTCTRL:
            print("RIGHTCTRL")
