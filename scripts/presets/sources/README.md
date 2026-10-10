# Sources the preset generators read

Tables a generator under `scripts/presets/` reads, copied here so the script runs from a
checkout alone. Each is a transcription of a vendor's published command set, kept as the
vendor spells it; the preset it makes names it in `SOURCES`.

- `digico/DiGiCo S OSC Commandset_OSCpaths.csv`, `digico/DiGiCo S OSC Commandset_channelNumbers.csv`:
  DiGiCo's S-series GP OSC command set - the methods, each marked for the strip kinds it applies to,
  and the channel numbers with the console's own commands - as S21_HiJack keeps them in its
  `Documentation/` (the author's own transcription from DiGiCo's GP OSC help). Tab-separated.
  Read by `digico_s.py`.
- `digico/digico-other-osc-2014.tsv`: DiGiCo's "OSC Command List for Other OSC" of 17 November 2014, the
  command set an SD or Quantum console's External Control panel loads, as text from the PDF S21_HiJack
  keeps (`DiGiCo_OTHER_OSC_List_17_11_14.pdf`): the path, the type, the minimum and the maximum. Read by
  `digico_sd.py`.
- `flux/spat-revolution-osc-table.tsv`: FLUX:: Immersive's "SPAT Revolution OSC Guidelines and Table" (the
  public spreadsheet linked from doc.flux.audio's Appendix C), its first sheet as text, one row per message.
  Read by `flux_spat.py`.
