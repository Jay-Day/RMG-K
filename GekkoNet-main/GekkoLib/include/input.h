#pragma once

#include "gekko_types.h"
#include "gekkonet.h"
#include <deque>
#include <memory>

namespace Gekko {
	struct GameInput {
	public:
		GameInput();

		void Init(GameInput* other);

		void Init(Frame frame_num, u8* inp, u32 inp_len);

		bool IsEqualTo(u8* other);

		void Clear();

	public:
		static const i32 NULL_FRAME = -1;

		Frame frame;

		std::unique_ptr<u8[]> input;

		u32 input_len;
	};

	struct InputBuffer {
		static const u32 DEFAULT_BUFF_SIZE = 128;

		InputBuffer();

        GekkoInputPredictor predictor = nullptr;
        GekkoInputObserver observer = nullptr;

		void Init(u8 delay, u8 input_window, u32 input_size, u32 buffer_size = DEFAULT_BUFF_SIZE);

		void AddLocalInput(Frame frame, u8* input);

		void AddInput(Frame frame, u8* input);

		void SetDelay(u8 delay);

		u8 GetDelay();
		
		void SetInputPredictionWindow(u8 input_window);

		Frame GetIncorrectPredictionFrame();

		std::unique_ptr<GameInput> GetInput(Frame frame, bool prediction = false);

		void SetRunaheadMode(bool running_ahead);

		Frame GetLastReceivedFrame();

        void ClearIncorrectFrames(Frame clear_limit);

	private:
		void ResetPrediction();

		u32 PreviousFrame(Frame frame);

		bool HandleInputPrediction(Frame frame);

		bool CanPredictInput();

	private:
		bool _running_ahead;

		u8 _input_delay;

		u8 _input_prediction_window;

		u32 _input_size;

		u32 _buff_size;

        std::unique_ptr<u8[]> _empty_input;

		Frame _last_received_input;

		Frame _last_predicted_input;

		Frame _first_predicted_input;

		std::deque<Frame> _incorrent_predicted_inputs;

		std::deque<std::unique_ptr<GameInput>> _inputs;
	};
}
