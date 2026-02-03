document.addEventListener('DOMContentLoaded', () => {
    
    // ICONS (SVG)
    const ICON_PLAY = `<svg width="40" height="40" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7z"/></svg>`;
    const ICON_PAUSE = `<svg width="40" height="40" viewBox="0 0 24 24" fill="currentColor"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg>`;
    const ICON_PLAY_SMALL = `<svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7z"/></svg>`;
    const ICON_PAUSE_SMALL = `<svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg>`;

    // GLOBALS
    let socket;
    let robots = [];
    let leaderboardData = [];
    let isGlobalPaused = true;
    let globalTimeMs = 300000; 
    let lastUpdate = Date.now(); 
    let lastHeartbeat = Date.now(); 
    const banner = document.getElementById('connectionBanner');

    // DOM ELEMENTS
    const globalPlayBtn = document.getElementById('globalPlayBtn');
    const globalResetBtn = document.getElementById('globalResetBtn');
    const globalTimeDisplay = document.getElementById('globalTimeDisplay');
    const robotGrid = document.getElementById('robotGrid');
    
    const viewToggle = document.getElementById('viewToggle');
    const leaderboardView = document.getElementById('leaderboardView');
    const leaderboardList = document.getElementById('leaderboardList');
    const filterMeanCheckbox = document.getElementById('filterMean');
    const deleteAllBtn = document.getElementById('deleteAllBtn');
    
    const timeModal = document.getElementById('timeModal');
    const inputMin = document.getElementById('inputMin');
    const inputSec = document.getElementById('inputSec');
    const modalSaveBtn = document.getElementById('modalSaveBtn');
    const modalCancelBtn = document.getElementById('modalCancelBtn');
    
    const resetModal = document.getElementById('resetModal');
    const btnResetSave = document.getElementById('btnResetSave');
    const btnResetNoSave = document.getElementById('btnResetNoSave');
    const btnResetCancel = document.getElementById('btnResetCancel');

    // --- INITIALISIERUNG ---
    if (localStorage.getItem('theme') === 'dark') document.body.classList.add('dark');
    document.getElementById('themeToggle').addEventListener('click', () => {
        document.body.classList.toggle('dark');
        localStorage.setItem('theme', document.body.classList.contains('dark') ? 'dark' : 'light');
    });

    viewToggle.addEventListener('change', (e) => {
        if (e.target.checked) {
            leaderboardView.classList.remove('hidden');
            robotGrid.classList.add('hidden');
            document.querySelector('.global-controls-section').classList.add('hidden');
            renderLeaderboard();
        } else {
            leaderboardView.classList.add('hidden');
            robotGrid.classList.remove('hidden');
            document.querySelector('.global-controls-section').classList.remove('hidden');
        }
    });

    filterMeanCheckbox.addEventListener('change', renderLeaderboard);
    deleteAllBtn.addEventListener('click', () => {
        if(confirm("Alle Daten unwiderruflich löschen?")) socket.send(JSON.stringify({cmd: "delete_all"}));
    });

    // --- GAME LOOP ---
    setInterval(() => {
        const now = Date.now();

        // 1. Connection Check Logik
        const timeSinceLastMsg = now - lastHeartbeat;

        if (timeSinceLastMsg > 10000) {
            // > 10 Sekunden: Rot
            if (banner.className !== 'error') {
                banner.textContent = "Verbindung verloren. Versuche reconnect...";
                banner.className = 'error';
            }
        } else if (timeSinceLastMsg > 3000) {
            // > 3 Sekunden: Orange
            if (banner.className !== 'warning') {
                banner.textContent = "Verbindung instabil";
                banner.className = 'warning';
            }
        } else {
            // Alles okay: Ausblenden
            if (banner.className !== '') {
                banner.className = '';
            }
        }

        // 2. Bestehende Timer Logik
        if (!isGlobalPaused && globalTimeMs > 0) {
            const delta = now - lastUpdate;
            lastUpdate = now;
            globalTimeMs -= delta;
            if(globalTimeMs < 0) globalTimeMs = 0;
            globalTimeDisplay.textContent = formatTime(globalTimeMs / 1000);
        } else {
            lastUpdate = Date.now();
        }

        // Blink Alarm
        if (globalTimeMs < 10000 && globalTimeMs > 0) {
            globalTimeDisplay.classList.add('blink-critical');
        } else {
            globalTimeDisplay.classList.remove('blink-critical');
        }

        // Editierbarkeit anzeigen (nur wenn Pause)
        if(isGlobalPaused) globalTimeDisplay.classList.add('editable');
        else globalTimeDisplay.classList.remove('editable');

        // Client Side Prediction Robots (Visuell)
        if(!isGlobalPaused) {
             robots.forEach(r => {
                if (r.running && r.time_left > 0) {
                     // Nur visuelles Update, genauer Sync kommt vom Server
                }
            });
        }
    }, 100);

    // --- WEBSOCKET ---
    function initWebSocket() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        socket = new WebSocket(protocol + '//' + window.location.hostname + '/ws');
        
        socket.onopen = () => {
            console.log("WebSocket Verbunden");
            lastHeartbeat = Date.now(); // Reset bei Verbindung
        };        

        socket.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                
                // WICHTIG: Nur Heartbeat updaten, NICHT lastUpdate!
                lastHeartbeat = Date.now();

                // 1. Global State
                if (typeof data.global_paused !== 'undefined') {
                    isGlobalPaused = data.global_paused;
                    
                    globalPlayBtn.innerHTML = isGlobalPaused ? ICON_PLAY : ICON_PAUSE;
                    // Button ist immer Orange (Primary), nur Icon wechselt
                    globalPlayBtn.className = "btn btn-primary btn-global-icon";
                    
                    globalResetBtn.disabled = !isGlobalPaused;
                    updateButtonsState();
                }
                
                // 2. Global Time
                if (typeof data.global_time !== 'undefined') {
                    // Sync nur wenn Differenz zu groß (>500ms)
                    if (Math.abs(globalTimeMs - data.global_time) > 500) {
                        globalTimeMs = data.global_time;
                        globalTimeDisplay.textContent = formatTime(globalTimeMs / 1000);
                    }
                }

                // 3. Robots
                if (data.robots) {
                    robots = data.robots;
                    if (!viewToggle.checked) renderRobots(robots);
                    
                    // ESP-Status-Icons aktualisieren
                    updateEspIcons(robots); 
                }

                // 4. Leaderboard
                if (data.leaderboard) {
                    leaderboardData = data.leaderboard;
                    if (viewToggle.checked) renderLeaderboard();
                }

            } catch (e) { console.error("WS Parse Error", e); }
        };
        socket.onclose = () => {
            console.log("WebSocket Disconnect. Retry...");
            setTimeout(initWebSocket, 2000);
        };
    }

    // --- HELPER ---
    function formatTime(sec) {
        if(sec < 0) sec = 0;
        const m = Math.floor(sec / 60).toString().padStart(2, '0');
        const s = (Math.floor(sec) % 60).toString().padStart(2, '0');
        return `${m}:${s}`;
    }

    function areAllNamesValid() {
        if (robots.length === 0) return false; 
        // Prüft, ob jedes Feld einen Wert hat und dieser nicht nur aus Leerzeichen besteht
        return robots.every(r => r.name && r.name.trim().length > 0);
    }

    // --- BUTTON STATE UPDATE (Logik komplett überarbeitet) ---
    function updateButtonsState() {
        const namesValid = areAllNamesValid();
        const errorMsg = "Bitte erst für alle Teams einen Namen eintragen!";

        // 1. GLOBAL PLAY BUTTON
        if (!namesValid) {
            // FALL A: Namen fehlen -> Button ausgegraut + Hinweis
            globalPlayBtn.disabled = true;
            globalPlayBtn.title = errorMsg; // <--- Dein gewünschter Hover-Hinweis
        } else {
            // FALL B: Alles okay -> Button klickbar
            globalPlayBtn.disabled = false;
            globalPlayBtn.title = isGlobalPaused ? "Spiel starten" : "Spiel pausieren";
        }

        // 2. ROBOT PLAY BUTTONS
        const robotBtns = document.querySelectorAll('.robot-controls .btn-play-small');
        robotBtns.forEach(btn => {
            if (!namesValid) {
                // FALL A: Namen fehlen -> Auch hier ausgegraut + Hinweis
                btn.disabled = true;
                btn.title = errorMsg;
            } else {
                // FALL B: Namen da -> Verhalten hängt vom Global-Status ab
                // Wenn Global Pause ist, sind die Roboter gesperrt (man muss erst global starten)
                btn.disabled = isGlobalPaused;
                btn.title = isGlobalPaused ? "Zuerst global starten" : "Start / Stop";
            }
        });
    }

    // --- RENDER ROBOTS ---
    function renderRobots(list) {
        const grid = document.getElementById('robotGrid');
        const currentIds = list.map(r => r.id);
        
        // Alte entfernen
        Array.from(grid.children).forEach(child => {
            if (!currentIds.includes(parseInt(child.dataset.id))) child.remove();
        });

        if(list.length === 0) {
            grid.innerHTML = '<div style="text-align: center; color: #aaa; padding: 2rem;">Warte auf Verbindung...</div>';
            return;
        }

        list.forEach(rob => {
            let card = grid.querySelector(`.robot-card[data-id="${rob.id}"]`);
            const statusClass = rob.online ? 'online' : 'offline';
            const pct = Math.min((rob.pieces / 36) * 100, 100);
            const btnIcon = rob.running ? ICON_PAUSE_SMALL : ICON_PLAY_SMALL;
            
            // VALIDIERUNG: Ist der Name leer?
            const isNameInvalid = !rob.name || rob.name.trim().length === 0;
            const nameInputClass = isNameInvalid ? "robot-name-input error" : "robot-name-input";

            if (!card) {
                card = document.createElement('div');
                card.className = 'robot-card';
                card.dataset.id = rob.id;
                // HTML Struktur
                card.innerHTML = `
                    <div class="robot-header">
                        <div class="status-dot-wrapper">
                            <div class="status-dot ${statusClass}"></div>
                            <div class="status-tooltip">${rob.origin || "Unbekannt"}</div>
                        </div>
                        <button class="icon-btn locate-icon" onclick="locateRobot(${rob.id})">
                             <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 10c0 7-9 13-9 13s-9-6-9-13a9 9 0 0 1 18 0z"></path><circle cx="12" cy="10" r="3"></circle></svg>
                        </button>
                        <input type="text" class="${nameInputClass}" value="${rob.name}" placeholder="Team Name" onchange="updateName(${rob.id}, this.value)">
                    </div>
                    <div class="robot-controls">
                        <button class="btn-play-small" onclick="toggleRobot(${rob.id})">${btnIcon}</button>
                        <div class="count-controls">
                            <button class="btn-minus" onclick="adjScore(${rob.id}, -1)">-</button>
                            <button class="btn-plus" onclick="adjScore(${rob.id}, 1)">+</button>
                        </div>
                    </div>
                    <div class="robot-time">${formatTime(rob.time_left / 1000)}</div>
                    <div class="robot-progress">
                        <div class="bar" style="width: ${pct}%"></div>
                        <div class="progress-text">${rob.pieces} / 36</div>
                    </div>
                `;
                grid.appendChild(card);
            } else {
                // UPDATE EXISTING
                card.querySelector('.status-dot').className = `status-dot ${statusClass}`;
                card.querySelector('.status-tooltip').textContent = rob.origin || "Unknown";
                
                const inp = card.querySelector('.robot-name-input');
                if (document.activeElement !== inp) inp.value = rob.name;
                
                // Klasse aktualisieren (Fehler rot / normal)
                inp.className = nameInputClass;

                const btn = card.querySelector('.btn-play-small');
                if(btn.innerHTML !== btnIcon) btn.innerHTML = btnIcon;
                
                card.querySelector('.robot-time').textContent = formatTime(rob.time_left / 1000);
                card.querySelector('.bar').style.width = `${pct}%`;
                card.querySelector('.progress-text').textContent = `${rob.pieces} / 36`;
            }
        });

        updateButtonsState();
    }

    // --- RENDER LEADERBOARD ---
    function renderLeaderboard() {
        const list = document.getElementById('leaderboardList');
        list.innerHTML = "";
        if(leaderboardData.length === 0) {
            list.innerHTML = "<div style='text-align:center; padding:2rem; color:#888;'>Keine Einträge</div>";
            return;
        }
        let displayData = [];
        const isMean = document.getElementById('filterMean').checked;
        
        if (isMean) {
            const groups = {};
            leaderboardData.forEach(e => {
                const n = e.name || "Unbekannt";
                if (!groups[n]) groups[n] = [];
                groups[n].push(e.pieces);
            });
            displayData = Object.keys(groups).map(n => {
                const arr = groups[n];
                const avg = arr.reduce((a,b)=>a+b,0) / arr.length;
                return { name: n, score: avg.toFixed(1), attempts: arr.length, isMean: true };
            }).sort((a,b) => b.score - a.score);
        } else {
            displayData = leaderboardData.map(e => ({ name: e.name, score: e.pieces, isMean: false }))
                          .sort((a,b) => b.score - a.score);
        }
        const ul = document.createElement('ul');
        ul.className = 'lb-list';
        displayData.forEach((e, i) => {
            const li = document.createElement('li');
            li.className = 'lb-item';
            li.innerHTML = `
                <div class="lb-rank">#${i+1}</div>
                <div class="lb-info"><div class="lb-name">${e.name}</div>${e.isMean?`<small>${e.attempts} Versuche</small>`:''}</div>
                <div class="lb-score">${e.score}</div>
            `;
            ul.appendChild(li);
        });
        list.appendChild(ul);
    }

    // --- RENDER ESP-STATUS_ICONS ---
    function updateEspIcons(robotList) {
        const grid = document.getElementById('espStatusGrid');
        if (!grid) return;

        // 1. Gruppieren nach Gerät (Origin)
        // Master und Clients haben jeweils eindeutige Origins.
        // Wir prüfen, ob IRGENDEIN Roboter dieses Geräts online ist.
        const devices = {};
        robotList.forEach(r => {
            if (!devices[r.origin]) {
                devices[r.origin] = { 
                    online: false, 
                    isMaster: r.origin === "Master ESP" 
                };
            }
            if (r.online) devices[r.origin].online = true;
        });

        // 2. Filtern und Sortieren
        // Master ist immer dabei. Clients nur, wenn online.
        const activeOrigins = Object.keys(devices).filter(k => {
            const d = devices[k];
            return d.isMaster || d.online;
        }).sort((a, b) => {
            // Master immer zuerst
            if (devices[a].isMaster) return -1;
            if (devices[b].isMaster) return 1;
            return a.localeCompare(b);
        });
        // 3. Auf 6 limitieren (2 Zeilen a 3 Spalten)
        const toShow = activeOrigins.slice(0, 6);

        // 4. SVG Definition
        const svgIcon = `
        <svg viewBox="0 0 24 24" width="100%" height="100%" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <rect x="7" y="2" width="10" height="20" rx="2" />
            <rect x="10" y="9" width="4" height="6" />
            <circle cx="4" cy="6" r="1.5" fill="currentColor" stroke="none"/>
            <circle cx="4" cy="12" r="1.5" fill="currentColor" stroke="none"/>
            <circle cx="4" cy="18" r="1.5" fill="currentColor" stroke="none"/>
            <circle cx="20" cy="6" r="1.5" fill="currentColor" stroke="none"/>
            <circle cx="20" cy="12" r="1.5" fill="currentColor" stroke="none"/>
            <circle cx="20" cy="18" r="1.5" fill="currentColor" stroke="none"/>
        </svg>
        `;

        // 5. HTML generieren
        grid.innerHTML = toShow.map(() => 
            `<div class="esp-icon active" title="Verbundenes ESP32 Modul">${svgIcon}</div>`
        ).join('');
    }

    // --- ACTIONS ---
    window.locateRobot = (id) => socket.send(JSON.stringify({cmd: "locate", id}));
    window.updateName = (id, val) => socket.send(JSON.stringify({cmd: "set_name", id, val}));
    window.toggleRobot = (id) => socket.send(JSON.stringify({cmd: "toggle_robot", id}));
    window.adjScore = (id, val) => socket.send(JSON.stringify({cmd: "adj_score", id, val}));
    
    // TIME EDIT (PRE-FILL VALUE)
    window.tryOpenGlobalTimeModal = function() {
        if(isGlobalPaused) {
            const secTotal = Math.floor(globalTimeMs / 1000);
            const m = Math.floor(secTotal / 60);
            const s = secTotal % 60;
            inputMin.value = m.toString().padStart(2, '0');
            inputSec.value = s.toString().padStart(2, '0');
            
            timeModal.classList.add('active');
        } else {
            // Feedback optional
        }
    };
    modalCancelBtn.onclick = () => timeModal.classList.remove('active');
    modalSaveBtn.onclick = () => {
        const m = parseInt(inputMin.value)||0;
        const s = parseInt(inputSec.value)||0;
        const total = m*60 + s;
        socket.send(JSON.stringify({cmd: "set_all_time", val: total}));
        timeModal.classList.remove('active');
    };

    // GLOBAL BUTTONS
    globalPlayBtn.onclick = () => {
        // KEIN Optimistic UI mehr hier! Wir warten auf Server.
        socket.send(JSON.stringify({cmd: "toggle_global"}));
    };
    
    // RESET LOGIK
    // HIER WAR DER FEHLER: resetModal ist oben schon definiert (const), darf nicht neu zugewiesen werden.
    globalResetBtn.onclick = () => resetModal.classList.add('active');
    
    btnResetSave.onclick = () => {
        socket.send(JSON.stringify({cmd: "reset_game", save: true}));
        resetModal.classList.remove('active');
    };
    btnResetNoSave.onclick = () => {
        socket.send(JSON.stringify({cmd: "reset_game", save: false}));
        resetModal.classList.remove('active');
    };
    btnResetCancel.onclick = () => resetModal.classList.remove('active');

    initWebSocket();
});