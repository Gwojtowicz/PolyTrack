#define PT_TRACE
#define main trk_main
#include "trk.cpp"
#undef main
int main(int argc, char** argv)
{
  std::vector<int> notes;
  for (int i = 1; i < argc; i++) notes.push_back(atoi(argv[i]));
  std::mt19937 rng(1234 + notes[0] * 7 + (int)notes.size());
  PolyTracker t; t.Init(SR);
  std::vector<KS> str(notes.size());
  std::vector<float> buf((int)(0.6 * SR), 0.0f);
  for (size_t k = 0; k < notes.size(); k++) { int st = (int)(k * 8e-3 * SR); str[k].Pluck(MidiToHz((float)notes[k]), DbToLin(-40), rng); for (int i = st; i < (int)buf.size(); i++) buf[i] += str[k].Tick(); }
  std::vector<NoteEvent> ev; int B = 96;
  for (int i = 0; i + B <= (int)buf.size(); i += B) { int ms = (int)((i + B) * 1000 / SR); t.mTrace = (ms == 400); if (t.mTrace) printf("t=%d\n", ms); t.Process(&buf[i], B, ev); }
}
