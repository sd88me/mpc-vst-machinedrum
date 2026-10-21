// Sends a kit table (KitModel.h) to an engine, only what changed: machines (a new machine starts from its own values when the user
// picked it, as on the unit), the 24 raw values, levels, mutes, LFOs, the master effects, the tempo; CTR-8P's targets. Kept apart from
// KitModel so that code which only edits tables (the Schwung module's plugin half) does not link the engine.
#pragma once
#include "KitModel.h"
#include "MdEngine.h"

namespace mm
{
	class EngineFeeder
	{
	public:
		struct Host
		{
			// The user changed track t's machine (not a kit or a restored state): true = the machine's own starting values are wanted.
			virtual bool takeDefaults(int t) = 0;
			// The starting values chosen for track t's new machine (SYN1-8; CTR-8P: all 24), already sent to the engine: show them.
			virtual void defaultsChosen(int t, const int* values, int count) = 0;
		protected:
			~Host() = default;
		};

		EngineFeeder() { reset(); }
		void reset();	// a new engine: everything is sent again, the master effects settle at once
		template<class E> void apply(E& engine, const KitValues& v, double bpm, Host& host);	// md::engine::Engine or ParallelEngine
		int applied(int t, int i) const { return m_raw[t][i]; }	// what the engine was last given for a track's raw value

	private:
		int m_machine[kTracks], m_raw[kTracks][kRaw], m_level[kTracks], m_lfo[kTracks][kNumLfo], m_master[kMasterBytes];
		double m_bpm = -1;
		bool m_masterSnap = true;	// the first values after an engine is built settle at once (as a unit that has been on a while)
	};
}
