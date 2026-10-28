#include "storage.h"

#include <new>

Gekko::StateStorage::StateStorage()
{
	_max_num_states = 0;
	_state_size = 0;
}


void Gekko::StateStorage::Init(u32 num_states, u32 state_size, bool limited)
{
	const u32 num = limited ? 2 : num_states + 2;
	_max_num_states = num;
	_state_size = state_size;

	for (u32 i = 0; i < _max_num_states; i++) {
		_states.push_back(std::make_unique<StateEntry>());
        _states.back().get()->state = std::make_unique<u8[]>(state_size);
		_states.back().get()->state_len = state_size;
	}

	_runahead_state.state = std::make_unique<u8[]>(state_size);
	_runahead_state.state_len = state_size;
	_runahead_state.frame = GameInput::NULL_FRAME;
}

bool Gekko::StateStorage::Grow(u32 num_states)
{
	const u32 num = num_states + 2;
	if (num <= _max_num_states) {
		return true;
	}

	// allocate first so a failure leaves the current ring intact. the buffers
	// are left uninitialized: zeroing them here would stall the frame that
	// grows the ring, and a slot is always saved into before it is loaded.
	std::vector<std::unique_ptr<StateEntry>> spare;
	try {
		for (u32 i = _max_num_states; i < num; i++) {
			auto entry = std::make_unique<StateEntry>();
			entry->state = std::unique_ptr<u8[]>(new u8[_state_size]);
			entry->state_len = _state_size;
			spare.push_back(std::move(entry));
		}
	}
	catch (const std::bad_alloc&) {
		return false;
	}

	// GetState maps a frame to slot frame % _max_num_states, so every saved
	// frame has to move to its slot in the larger ring. the saved frames are
	// consecutive and fewer than the new size, so they never collide; if they
	// somehow did, the newer frame is the one a rollback can still ask for.
	std::vector<std::unique_ptr<StateEntry>> grown(num);
	for (auto& entry : _states) {
		if (entry->frame != GameInput::NULL_FRAME) {
			auto& slot = grown[entry->frame % num];
			if (slot && slot->frame > entry->frame) {
				entry->frame = GameInput::NULL_FRAME;
			}
			else {
				if (slot) {
					slot->frame = GameInput::NULL_FRAME;
					spare.push_back(std::move(slot));
				}
				slot = std::move(entry);
				continue;
			}
		}
		spare.push_back(std::move(entry));
	}
	for (auto& slot : grown) {
		if (!slot) {
			slot = std::move(spare.back());
			spare.pop_back();
		}
	}

	_states = std::move(grown);
	_max_num_states = num;
	return true;
}

Gekko::StateEntry* Gekko::StateStorage::GetRunaheadState()
{
	return &_runahead_state;
}

Gekko::StateEntry* Gekko::StateStorage::GetState(Frame frame)
{
	frame = frame < 0 ? frame + _max_num_states : frame;
	return _states[frame % _max_num_states].get();
}
