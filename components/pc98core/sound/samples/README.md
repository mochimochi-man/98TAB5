# Compact orchestra sample bank

Derived from VSCO 2 Community Edition (CC0-1.0).
Recorded by Sam Gossner & Simon Dalzell; original sample cutting by Elan Hickler / Soundemote.
Upstream: https://github.com/sgossner/VSCO-2-CE
Publisher: https://versilian-studios.com/vsco-community/

Violin, viola and cello section and solo contrabass sustains with vibrato at about five-semitone steps, each note in two
independent recordings (two dynamic layers), and harp one-shots; horn, trumpet, trombone, flute, oboe, clarinet
and bassoon sustains, pizzicato basses and celli and glockenspiel one-shots, one recording a note. Adaptations: mono
conversion, 22050 Hz resampling, attack trimming, DC removal, level normalization, crossfaded sustain loops whose slow
level swell is flattened and whose harmonics are each held at their own level (so a short loop does not repeat the
players' drifting as a wave), and one-shot fade-outs. No source game audio is included.

SOURCES.json records the exact upstream revision, file hashes, measured tuning and the loop swell before / after.
LICENSE contains the upstream CC0 dedication. The normal build is offline and uses the committed bank directly.
Regeneration requires Python, NumPy and FFmpeg; see tools/generate_orchestra_bank.py.
