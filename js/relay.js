// Relay: a sensor on an MQTT broker fires a cue, and a light follows.
//
// Hears a temperature published on room1/temp by a device whose prefix is
// /house (Show settings > Network: an MQTT device), fires cue 3.1 when it
// rises past 25 and 3.2 when it falls back under 23 - a band, so a reading
// that hovers fires nothing twice - and publishes 1 or 0 on lights/1 to say
// which side it is on. Change the addresses to your device's prefix and
// topics, and the numbers to your line.

let above = false;

godot.hear('/house/room1/temp', (t) => {
    if (!above && t > 25) {
        above = true;
        godot.fire('3.1');
        godot.send('/house/lights/1', 1);
    } else if (above && t < 23) {
        above = false;
        godot.fire('3.2');
        godot.send('/house/lights/1', 0);
    }
});
