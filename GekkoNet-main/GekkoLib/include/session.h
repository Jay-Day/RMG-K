#pragma once

#include <vector>
#include <map>
#include <memory>

#include "gekkonet.h"
#include "gekko_types.h"
#include "backend.h"
#include "event.h"
#include "sync.h"
#include "storage.h"

// define GekkoSession internally
struct GekkoSession {
    virtual void Init(GekkoConfig* config) = 0;
    virtual void SetLocalDelay(i32 player, u8 delay) = 0;
    virtual bool SetPredictionWindow(u8 window) = 0;
    virtual void SetRunahead(u8 runahead) = 0;
    virtual void SetNetAdapter(GekkoNetAdapter* adapter) = 0;
    virtual i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) = 0;
    virtual void AddLocalInput(i32 player, void* input) = 0;
    virtual GekkoGameEvent** UpdateSession(i32* count) = 0;
    virtual GekkoSessionEvent** Events(i32* count) = 0;
    virtual f32 FramesAhead() = 0;
    virtual void NetworkStats(i32 player, GekkoNetworkStats* stats) = 0;
    virtual void NetworkPoll() = 0;
    virtual void DisconnectPlayer(i32 handle) = 0;
    virtual void SetDisconnectTimeout(u32 timeout_ms) = 0;
    virtual ~GekkoSession() = default;
};

namespace Gekko {

	class GameSession : public GekkoSession {
    public:
        GameSession();

        void Init(GekkoConfig* config) override;

        void SetLocalDelay(i32 player, u8 delay) override;

        bool SetPredictionWindow(u8 window) override;

        void SetRunahead(u8 runahead) override;

        void SetNetAdapter(GekkoNetAdapter* adapter) override;

        i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) override;

        void AddLocalInput(i32 player, void* input) override;

        GekkoGameEvent** UpdateSession(i32* count) override;

        GekkoSessionEvent** Events(i32* count) override;

        f32 FramesAhead() override;

        void NetworkStats(i32 player, GekkoNetworkStats* stats) override;

        void NetworkPoll() override;

        void DisconnectPlayer(i32 handle) override;

        void SetDisconnectTimeout(u32 timeout_ms) override;

	private:
		void Poll();

		bool AllActorsValid();

		void HandleReceivedInputs();

		void SendLocalInputs();

		u8 GetMinLocalDelay();

		bool IsPlayingLocally();

        bool IsLockstepActive() const;

		void AddDisconnectedPlayerInputs();

		void SendSpectatorInputs();

		void HandleRollback();

		void HandleSavingConfirmedFrame();

		void HandleRunahead();

		void RewindRunahead();

        void SendSessionHealthCheck();

        void SendNetworkHealthCheck();

        void SessionIntegrityCheck();

	private:
		bool _started;

		Frame _last_saved_frame;

        Frame _last_sent_healthcheck;

		Frame _runahead_start_frame;

		u8 _runahead_frames;

		// largest prediction window this session has run with. predictions made
		// under it may still be pending, so the state storage stays sized for it
		// and frames older than it are the only ones known to be confirmed.
		u8 _max_prediction_window;

		std::unique_ptr<u8[]> _disconnected_input;

		GekkoConfig _config;

		SyncSystem _sync;

        GekkoNetAdapter* _host;

		MessageSystem _msg;

		StateStorage _storage;

        GameEventSystem _game_events;
	};

	class SpectatorSession : public GekkoSession {
    public:
        SpectatorSession();

        void Init(GekkoConfig* config) override;

        void SetLocalDelay(i32 player, u8 delay) override;

        // Spectators never predict.
        bool SetPredictionWindow(u8 window) override { (void)window; return false; }

        void SetRunahead(u8 runahead) override {}

        void SetNetAdapter(GekkoNetAdapter* adapter) override;

        i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) override;

        void AddLocalInput(i32 player, void* input) override;

        GekkoGameEvent** UpdateSession(i32* count) override;

        GekkoSessionEvent** Events(i32* count) override;

        f32 FramesAhead() override;

        void NetworkStats(i32 player, GekkoNetworkStats* stats) override;

        void NetworkPoll() override;

        // Spectators don't drive player connectivity; nothing to force-drop.
        void DisconnectPlayer(i32 handle) override { (void)handle; }

        void SetDisconnectTimeout(u32 timeout_ms) override { (void)timeout_ms; }

	private:
		void Poll();

		bool AllActorsValid();

		void HandleReceivedInputs();

        bool ShouldDelaySpectator();

	private:
		bool _started;

        bool _delay_spectator;

		Frame _last_saved_frame;

		GekkoConfig _config;

		SyncSystem _sync;

        GekkoNetAdapter* _host;

		MessageSystem _msg;

        GameEventSystem _game_events;
	};

    class StressSession : public GekkoSession {
    public:
        StressSession();

        void Init(GekkoConfig* config) override;

        void SetLocalDelay(i32 player, u8 delay) override;

        // Stress sessions roll back a fixed check distance, not a prediction window.
        bool SetPredictionWindow(u8 window) override { (void)window; return false; }

        void SetRunahead(u8 runahead) override {}

        void SetNetAdapter(GekkoNetAdapter* adapter) override;

        i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) override;

        void AddLocalInput(i32 player, void* input) override;

        GekkoGameEvent** UpdateSession(i32* count) override;

        GekkoSessionEvent** Events(i32* count) override;

        f32 FramesAhead() override;

        void NetworkStats(i32 player, GekkoNetworkStats* stats) override;

        void NetworkPoll() override;

        // No network actors in a stress session.
        void DisconnectPlayer(i32 handle) override { (void)handle; }

        void SetDisconnectTimeout(u32 timeout_ms) override { (void)timeout_ms; }

    private:
        void HandleRollback();

        void CheckForDesyncs(Frame check_frame);

    private:
        GekkoConfig _config;

        SyncSystem _sync;

        StateStorage _storage;

        SessionEventSystem _session_events;

        GameEventSystem _game_events;

        std::vector<Player> _locals;

        u32 _check_distance;

        std::map<Frame, u32> _checksum_history;
    };
}
