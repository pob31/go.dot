# Sources the preset generators read

Tables a generator under `scripts/presets/` reads, copied here so the script runs from a
checkout alone. Each is a transcription of a vendor's published command set, kept as the
vendor spells it; the preset it makes names it in `SOURCES`.

- `digico/DiGiCo S OSC Commandset_OSCpaths.csv`, `digico/DiGiCo S OSC Commandset_channelNumbers.csv`:
  DiGiCo's S-series GP OSC command set - the methods, each marked for the strip kinds it applies to,
  and the channel numbers with the console's own commands - as S21_HiJack keeps them in its
  `Documentation/` (the author's own transcription from DiGiCo's GP OSC help). Tab-separated.
  Read by `digico_s.py`.
