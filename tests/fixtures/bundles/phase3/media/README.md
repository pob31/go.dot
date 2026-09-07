# `media/`

Empty in the repository, and deliberately.

The black-box driver (`tests/blackbox/first_sound.py`) writes `tone.wav` into its own
throwaway copy of this bundle before it starts, using Python's `wave` module. Two reasons, and
the second is the load-bearing one.

**No binary in git.** A committed WAV is a blob nobody can review, that no diff can explain,
and that grows the clone for everybody who will never listen to it.

**The driver has to know what is in it, exactly.** The file it writes is a constant — every
sample the same known value — so the assertions afterwards are arithmetic rather than
tolerance: a routing coefficient of 0.5 means the right output carries exactly half, a fade to
−20 dB means exactly a tenth, and silence means zeros and not "small". A file somebody
committed once and nobody can regenerate could not be asserted against that tightly, and the
first time it was replaced the tests would start measuring the replacement.

A show that references a file the bundle does not have is reported at the arm and never at the
load (`run.failed media-missing`), so this bundle opens perfectly well with nothing here.
