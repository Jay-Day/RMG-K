#include "input.h"

#include <cstdlib> 
#include <cstring>

Gekko::InputBuffer::InputBuffer() {
    _empty_input = nullptr;

	_input_size = 0;
	_buff_size = DEFAULT_BUFF_SIZE;
	_input_delay = 0;
	_input_prediction_window = 0;
    _running_ahead = false;

	_last_received_input = GameInput::NULL_FRAME;
	_last_predicted_input = GameInput::NULL_FRAME;
	_first_predicted_input = GameInput::NULL_FRAME;

    _incorrent_predicted_inputs.clear();
}

void Gekko::InputBuffer::Init(u8 delay, u8 input_window, u32 input_size, u32 buffer_size)
{
	_input_delay = delay;
	_input_size = input_size;
    _buff_size = buffer_size;
	_input_prediction_window = input_window;

	_last_received_input = GameInput::NULL_FRAME;

	_last_predicted_input = GameInput::NULL_FRAME;
	_first_predicted_input = GameInput::NULL_FRAME;

    _incorrent_predicted_inputs.clear();

	// init GameInput array
    _empty_input = std::make_unique<u8[]>(_input_size);
    std::memset(_empty_input.get(), 0, _input_size);

	for (u32 i = 0; i < _buff_size; i++) {
        _inputs.push_back(std::make_unique<GameInput>());
		_inputs[i]->Init(GameInput::NULL_FRAME, _empty_input.get(), _input_size);
	}
}

void Gekko::InputBuffer::AddLocalInput(Frame frame, u8* input)
{
	if (_inputs[frame % _buff_size]->frame == GameInput::NULL_FRAME && _input_delay > 0) {
		for (i32 i = 0; i < _input_delay; i++) {
			AddInput(i,  _empty_input.get());
		}
	}
	AddInput(frame + _input_delay, input);
}

void Gekko::InputBuffer::AddInput(Frame frame, u8* input)
{
    // only allow sequential input insertion
    if (frame != _last_received_input + 1) {
        return;
    }

    const Frame idx = frame % _buff_size;
    if (_input_prediction_window > 0 && _first_predicted_input == frame) {
        if (!_inputs[idx]->IsEqualTo(input)) {
            // incorrect prediction
            _incorrent_predicted_inputs.push_back(_first_predicted_input);

            // last prediction frame ? add correct input and reset prediction
            if (_first_predicted_input == _last_predicted_input) {
                _inputs[idx]->Init(frame, input, _input_size);
                ResetPrediction();
            } else {

                // repeat the correct inputs instead of the wrong one.
                const Frame diff = _last_predicted_input - _first_predicted_input;
                for (Frame i = 0; i <= diff; i++) {
                    const Frame pred_frame = _first_predicted_input + i;
                    _inputs[pred_frame % _buff_size]->Init(pred_frame, input, _input_size);
                }

                // move along this frame since the previous is verified now.
                _first_predicted_input++;
            }
        } else {

            // correct prediction
            if (_first_predicted_input == _last_predicted_input) {
                ResetPrediction();
            } else {
                _first_predicted_input++;
            }
        }
    } else {
        // not a predicted input
        _inputs[idx]->Init(frame, input, _input_size);
    }

    // always advance the buffer
    _last_received_input++;
}

void Gekko::InputBuffer::SetDelay(u8 delay)
{
	// early return AddLocalInput will handle it
	if (_last_received_input == GameInput::NULL_FRAME || _input_delay == delay) {
		_input_delay = delay;
		return;
	}

	// when our current delay is smaller then the new delay 
	// all we have to do is expand the delay with the last input we received
	if (_input_delay < delay) {
		_input_delay = delay;

		Frame last_input = _last_received_input;
        u8* prev = _inputs[last_input % _buff_size]->input.get();

		for (i32 i = 1; i <= _input_delay; i++) {
			AddInput(last_input + i, prev);
		}

		return;
	}

	// if our current delay is bigger then our new delay the plan is different. 
	// since we probably already sent the inputs up to _last_received_input 
	// which includes the previous delay we should not modify any inputs that happend before that
	// this will probably mean that it will drop local inputs until the local machine catches up
	if (_input_delay > delay) {
		_input_delay = delay;
		return;
	}
}

u8 Gekko::InputBuffer::GetDelay() 
{
	return _input_delay;
}

void Gekko::InputBuffer::SetInputPredictionWindow(u8 input_window)
{
	_input_prediction_window = input_window;
}

Frame Gekko::InputBuffer::GetIncorrectPredictionFrame()
{
	return _incorrent_predicted_inputs.empty() ? GameInput::NULL_FRAME : _incorrent_predicted_inputs.front();
}

void Gekko::InputBuffer::ResetPrediction()
{
	_first_predicted_input = GameInput::NULL_FRAME;
    _last_predicted_input = GameInput::NULL_FRAME;
}

Frame Gekko::InputBuffer::GetLastReceivedFrame()
{
	return _last_received_input;
}

void Gekko::InputBuffer::ClearIncorrectFrames(Frame clear_limit)
{
    while (!_incorrent_predicted_inputs.empty() && _incorrent_predicted_inputs.front() <= clear_limit) {
        _incorrent_predicted_inputs.pop_front();
    }
}


bool Gekko::InputBuffer::HandleInputPrediction(Frame frame)
{
	const u32 prev_input = PreviousFrame(frame);
	if (_first_predicted_input == GameInput::NULL_FRAME) {
		// if the prev frame happends to be an empty frame add a dummy input
		if (_inputs[prev_input % _buff_size]->frame == GameInput::NULL_FRAME) {
			_inputs[frame % _buff_size]->Init(frame, _empty_input.get(), _input_size);
		}
		else {
			// predict by copying the previous input
			_inputs[frame % _buff_size]->Init(_inputs[prev_input % _buff_size].get());
			_inputs[frame % _buff_size]->frame = frame;
		}
		// set prediction values
		_first_predicted_input = frame;
		_last_predicted_input = frame;
		return true;
	}
	else {
		// continue predicting if the diff is within the window
		const u32 diff = _last_predicted_input - _first_predicted_input + 1;
		if (_input_prediction_window > diff) {
			_inputs[frame % _buff_size]->Init(_inputs[prev_input % _buff_size].get());
			_inputs[frame % _buff_size]->frame = frame;
			// move the last predicted input along with the requested frame
			_last_predicted_input = frame;
			return true;
		}
		return false;
	}
}

bool Gekko::InputBuffer::CanPredictInput() {
	const Frame diff = std::abs(_last_predicted_input) - std::abs(_first_predicted_input) + 1;
	return _input_prediction_window > 0 && diff < _input_prediction_window;
}

u32 Gekko::InputBuffer::PreviousFrame(Frame frame)
{
	return frame - 1 < 0 ? _buff_size - frame - 1 : frame - 1;
}

void Gekko::InputBuffer::SetRunaheadMode(bool running_ahead)
{
    _running_ahead = running_ahead;
}

std::unique_ptr<Gekko::GameInput> Gekko::InputBuffer::GetInput(Frame frame, bool prediction)
{
    auto inp = std::make_unique<GameInput>();

	if (_last_received_input < frame) {
		// Frame 0 is the shared starting point for every peer. Do not predict it;
		// wait until the real input has arrived before allowing emulation to start.
		if (frame == 0) {
			return inp;
		}

		// no input? check if we should predict the input
		if (prediction) {
            if (_last_predicted_input != GameInput::NULL_FRAME &&
                frame <= _last_predicted_input) {
                // return existing prediction
                inp->Init(_inputs[frame % _buff_size].get());

            } else if (!_running_ahead && CanPredictInput() && HandleInputPrediction(frame)) {
                // generate new prediction
				inp->Init(_inputs[frame % _buff_size].get());

            } else if (_running_ahead) {
                // return last known input without advancing prediction state
                const Frame ref = _last_predicted_input != GameInput::NULL_FRAME ? _last_predicted_input : _last_received_input;
                if (ref != GameInput::NULL_FRAME) {
                    inp->Init(_inputs[ref % _buff_size].get());
                }
            }
		}
		return inp;
	}

    if (_inputs[frame % _buff_size]->frame != frame ||
        _inputs[frame % _buff_size]->frame == GameInput::NULL_FRAME) {
        return inp;
    }

	inp->Init(_inputs[frame % _buff_size].get());
	return inp;
}

void Gekko::GameInput::Init(GameInput* other)
{
	frame = other->frame;
	input_len = other->input_len;

	if (input) {
		std::memcpy(input.get(), other->input.get(), input_len);
		return;
	}

	input = std::make_unique<u8[]>(input_len);

    if (input) {
        std::memcpy(input.get(), other->input.get(), input_len);
    }
}

void Gekko::GameInput::Init(Frame frame_num, u8* inp, u32 inp_len)
{
	frame = frame_num;
	input_len = inp_len;

	if (input) {
		std::memcpy(input.get(), inp, input_len);
		return;
	}

	input = std::make_unique<u8[]>(input_len);

    if (input) {
        std::memcpy(input.get(), inp, input_len);
    }
}

bool Gekko::GameInput::IsEqualTo(u8* other)
{
	return input_len != 0 && std::memcmp(input.get(), other, input_len) == 0;
}

void Gekko::GameInput::Clear()
{
    std::memset(input.get(), 0, input_len);
	frame = NULL_FRAME;
}

Gekko::GameInput::GameInput()
{
	frame = NULL_FRAME;
	input_len = 0;
	input = std::unique_ptr<u8[]>();
}
