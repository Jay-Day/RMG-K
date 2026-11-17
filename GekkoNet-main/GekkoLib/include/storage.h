#pragma once

#include "gekko_types.h"
#include "input.h"

#include <memory>
#include <vector>

namespace Gekko {
	struct StateEntry {
		Frame frame = GameInput::NULL_FRAME;
		std::unique_ptr<u8[]> state;
		u32 state_len = 0;
		u32 checksum = 0;
	};

	class StateStorage {
	public:
		StateStorage();

		void Init(u32 num_states, u32 state_size, bool limited);

		// Grow the ring so it can hold a larger prediction window while keeping
		// every state already saved. Never shrinks. Returns false, leaving the
		// storage untouched, when the extra states cannot be allocated.
		bool Grow(u32 num_states);

		StateEntry* GetState(Frame frame);

		StateEntry* GetRunaheadState();

	private:
		u32 _max_num_states;

		u32 _state_size;

		std::vector<std::unique_ptr<StateEntry>> _states;

		StateEntry _runahead_state;
	};
}
