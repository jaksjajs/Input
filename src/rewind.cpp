#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

using namespace geode::prelude;

// Rewind instantáneo al morir (BORRADOR SIN PROBAR):
// 1. Cada pocos pasos de física guarda un snapshot del nivel (checkpoint de práctica)
//    y registra todos tus clics/soltadas con el paso en que ocurrieron.
// 2. Cuando vas a morir y hiciste un clic hace muy poco, el mod cancela la muerte y,
//    al final de ese mismo frame, prueba en silencio varias versiones con ese último
//    clic corrido unos pasos. Cada versión se simula a toda velocidad, sin dibujar.
// 3. La primera que sobrevive se queda y sigues jugando normal. Si ninguna sobrevive,
//    se repite tu jugada original hasta que mueres, como si nada.

namespace {
	constexpr int kSnapEvery = 4;  // pasos entre snapshots
	constexpr int kMargin = 4;     // pasos extra después de la muerte para dar un intento por bueno
	constexpr int kMaxAge = 80;    // snapshots/eventos más viejos que esto se descartan
	constexpr int kRecent = 24;    // el último clic tiene que ser de hace menos de esto
	constexpr float kStepDt = 1.f / 240.f;  // OJO: asume 240 TPS (sin TPS bypass)

	struct ClickEvent {
		int step;
		bool down;
		int button;
		bool p1;
	};

	struct Snap {
		int step;
		Ref<CheckpointObject> cp;
		bool held[2][4];
	};

	struct State {
		int step = 0;
		std::deque<ClickEvent> events;
		std::deque<Snap> snaps;
		bool gameHeld[2][4] = {};
		bool liveHeld[2][4] = {};
		bool injecting = false;

		bool pending = false;     // hay una muerte cancelada esperando corrección
		bool ff = false;          // simulando a toda velocidad
		bool ffDied = false;      // murió durante la simulación
		bool allowDeath = false;  // dejar que la muerte ocurra de verdad
		int snapStep = 0;
		int windowEnd = 0;
		std::vector<ClickEvent> base;                // jugada original
		std::vector<std::vector<ClickEvent>> cands;  // versiones corregidas
		const std::vector<ClickEvent>* current = nullptr;

		void clear() {
			step = 0;
			events.clear();
			snaps.clear();
			std::memset(gameHeld, 0, sizeof(gameHeld));
			std::memset(liveHeld, 0, sizeof(liveHeld));
			injecting = false;
			pending = false;
			ff = false;
			ffDied = false;
			allowDeath = false;
			base.clear();
			cands.clear();
			current = nullptr;
		}
	};

	State g;

	bool rewindOn() {
		return Mod::get()->getSettingValue<bool>("rewind-enabled");
	}

	int maxShift() {
		return static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-shift"));
	}

	void takeSnapshot(PlayLayer* pl) {
		for (auto& s : g.snaps) {
			if (s.step == g.step) return;
		}
		auto cp = pl->createCheckpoint();
		if (!cp) return;

		Snap s;
		s.step = g.step;
		s.cp = cp;
		std::memcpy(s.held, g.gameHeld, sizeof(s.held));
		g.snaps.push_back(std::move(s));

		while (!g.snaps.empty() && g.snaps.front().step < g.step - kMaxAge) g.snaps.pop_front();
		while (!g.events.empty() && g.events.front().step < g.step - kMaxAge) g.events.pop_front();
	}

	// Vuelve al snapshot guardado en g.snapStep.
	bool restoreSnapshot(PlayLayer* pl) {
		Ref<CheckpointObject> cp;
		int sstep = 0;
		bool held[2][4] = {};
		bool found = false;
		for (auto& s : g.snaps) {
			if (s.step == g.snapStep) {
				cp = s.cp;
				sstep = s.step;
				std::memcpy(held, s.held, sizeof(held));
				found = true;
			}
		}
		if (!found) return false;

		pl->loadFromCheckpoint(cp);
		g.step = sstep;
		while (!g.events.empty() && g.events.back().step >= sstep) g.events.pop_back();
		while (!g.snaps.empty() && g.snaps.back().step > sstep) g.snaps.pop_back();

		// Dejar los botones como estaban en el snapshot.
		for (int p = 0; p < 2; p++) {
			auto player = p == 0 ? pl->m_player1 : pl->m_player2;
			for (int b = 0; b < 4; b++) {
				g.gameHeld[p][b] = held[p][b];
				if (!player) continue;
				// OJO: el nombre/tipo de m_holdingButtons puede variar según la versión de los bindings.
				if (player->m_holdingButtons[b] != held[p][b]) {
					g.injecting = true;
					pl->handleButton(held[p][b], b, p == 0);
					g.injecting = false;
				}
			}
		}
		return true;
	}

	// Alinear el estado de los botones del juego con lo que el jugador sostiene de verdad.
	void syncLiveButtons(PlayLayer* pl) {
		for (int p = 0; p < 2; p++) {
			for (int b = 0; b < 4; b++) {
				if (g.gameHeld[p][b] != g.liveHeld[p][b]) {
					g.injecting = true;
					pl->handleButton(g.liveHeld[p][b], b, p == 0);
					g.injecting = false;
				}
			}
		}
	}
}

class $modify(RewindBase, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		auto pl = PlayLayer::get();
		if (!pl || static_cast<GJBaseGameLayer*>(pl) != this || !rewindOn()
			|| button < 0 || button > 3) {
			return GJBaseGameLayer::handleButton(down, button, isPlayer1);
		}

		int p = isPlayer1 ? 0 : 1;
		if (!g.injecting) {
			g.liveHeld[p][button] = down;
			if (g.ff) return;  // durante la simulación no entran clics reales
		}

		g.gameHeld[p][button] = down;
		g.events.push_back({g.step, down, button, isPlayer1});
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		auto pl = PlayLayer::get();
		if (!pl || static_cast<GJBaseGameLayer*>(pl) != this || !rewindOn()) {
			return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		}

		if (g.ff) {
			// Simulación: meter los clics de la versión que se está probando.
			if (g.current) {
				auto evs = *g.current;
				for (auto& e : evs) {
					if (e.step == g.step) {
						g.injecting = true;
						pl->handleButton(e.down, e.button, e.p1);
						g.injecting = false;
					}
				}
			}
		} else if (g.step % kSnapEvery == 0) {
			takeSnapshot(pl);
		}

		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		g.step++;
	}
};

class $modify(RewindPlayLayer, PlayLayer) {
	void resetLevel() {
		g.clear();
		PlayLayer::resetLevel();
	}

	void update(float dt) {
		PlayLayer::update(dt);
		// La corrección se hace al final del mismo frame, antes de dibujar nada.
		if (g.pending && !g.ff && rewindOn()) this->instantRewind();
	}

	void runUntil(int endStep) {
		int guard = 0;
		while (g.step <= endStep && !g.ffDied && guard++ < 2000) {
			PlayLayer::update(kStepDt);
		}
	}

	void instantRewind() {
		g.pending = false;
		int endStep = std::max(g.windowEnd, g.step);  // nunca perder tiempo ya jugado
		g.ff = true;

		bool survived = false;
		for (size_t i = 0; i < g.cands.size() && !survived; i++) {
			if (!restoreSnapshot(this)) break;
			g.ffDied = false;
			g.current = &g.cands[i];
			this->runUntil(endStep);
			survived = !g.ffDied;
		}

		if (!survived) {
			// Ninguna versión sirvió: repetir la jugada original hasta que muera de verdad.
			if (restoreSnapshot(this)) {
				g.ffDied = false;
				g.allowDeath = true;
				g.current = &g.base;
				this->runUntil(endStep);
				g.allowDeath = false;
			}
		}

		g.ff = false;
		g.current = nullptr;
		g.cands.clear();
		if (survived) syncLiveButtons(this);
	}

	void destroyPlayer(PlayerObject* player, GameObject* obj) {
		if (!rewindOn() || obj == m_anticheatSpike) {
			return PlayLayer::destroyPlayer(player, obj);
		}

		// Muerte durante una simulación.
		if (g.ff) {
			g.ffDied = true;
			if (g.allowDeath) return PlayLayer::destroyPlayer(player, obj);
			return;
		}
		if (g.pending) return;

		// Hace falta un clic reciente que corregir.
		if (g.events.empty()) return PlayLayer::destroyPlayer(player, obj);
		int D = g.step;
		ClickEvent last = g.events.back();
		int shift = maxShift();
		if (D - last.step > kRecent) return PlayLayer::destroyPlayer(player, obj);

		// Último snapshot que quede antes del clic más el máximo corrimiento hacia atrás.
		int target = last.step - shift;
		const Snap* snap = nullptr;
		for (auto& s : g.snaps) {
			if (s.step <= target) snap = &s;
		}
		if (!snap) return PlayLayer::destroyPlayer(player, obj);
		int snapStep = snap->step;

		std::vector<ClickEvent> base;
		for (auto& e : g.events) {
			if (e.step >= snapStep) base.push_back(e);
		}
		if (base.empty()) return PlayLayer::destroyPlayer(player, obj);

		std::vector<std::vector<ClickEvent>> cands;
		for (int k = 1; k <= shift; k++) {
			for (int sign : {1, -1}) {
				int ns = last.step + sign * k;
				if (ns < snapStep) continue;
				if (base.size() >= 2 && ns <= base[base.size() - 2].step) continue;
				auto c = base;
				c.back().step = ns;
				cands.push_back(std::move(c));
			}
		}
		if (cands.empty()) return PlayLayer::destroyPlayer(player, obj);

		g.base = std::move(base);
		g.cands = std::move(cands);
		g.snapStep = snapStep;
		g.windowEnd = std::max(D, last.step + shift) + kMargin;
		g.pending = true;
		// Sin llamar al original: la muerte se cancela y se resuelve al final del frame.
	}
};
