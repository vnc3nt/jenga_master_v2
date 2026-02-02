document.addEventListener('DOMContentLoaded', () => {
    
    // ICONS
    const ICON_PLAY = `<svg width="40" height="40" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7z"/></svg>`;
    const ICON_PAUSE = `<svg width="40" height="40" viewBox="0 0 24 24" fill="currentColor"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg>`;
    const ICON_PLAY_SMALL = `<svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7z"/></svg>`;
    const ICON_PAUSE_SMALL = `<svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg>`;

    // Globals
    let socket;
    let robots = [];
    let leaderboardData = [];
    let isGlobalPaused = true;
    let globalTimeMs = 300000; 
    let lastUpdate = Date.now(); 

    const globalPlayBtn = document.getElementById('globalPlayBtn');
    const globalResetBtn = document.getElementById('globalResetBtn');
    const globalTimeDisplay = document.getElementById('globalTimeDisplay');
    const robotGrid = document.getElementById('robotGrid');
    
    // UI Setup
    if (localStorage.getItem('theme') === 'dark') document.body.classList.add('dark');
    document.getElementById('themeToggle').addEventListener('click', () => {
        document.body.classList.toggle('dark');
        localStorage.setItem('theme', document.body.classList.contains('dark') ? 'dark' : 'light');
    });

    const viewToggle = document.getElementById('viewToggle');
    const leaderboardView = document.getElementById('leaderboardView');
    const leaderboardList = document.getElementById('leaderboardList');
    const filterMeanCheckbox = document.getElementById('filterMean');
    const deleteAllBtn = document.getElementById('deleteAllBtn');
    
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

    // Modals
    const timeModal = document.getElementById('timeModal');
    const inputMin = document.getElementById('inputMin');
    const inputSec = document.getElementById('inputSec');
    const modalSaveBtn = document.getElementById('modalSaveBtn');
    const modalCancelBtn = document.getElementById('modalCancelBtn');
    
    const resetModal = document.getElementById('resetModal');
    const btnResetSave = document.getElementById('btnResetSave');
    const btnResetNoSave = document.getElementById('btnResetNoSave');
    const btnResetCancel = document.getElementById('btnResetCancel');

    // --- GAME LOOP ---
    setInterval(() => {
        if (!isGlobalPaused && globalTimeMs > 0) {
            const now = Date.now();
            const delta = now - lastUpdate;
            lastUpdate = now;
            globalTimeMs -= delta;
            if(globalTimeMs < 0) globalTimeMs = 0;
            globalTimeDisplay.textContent = formatTime(globalTimeMs / 1000);
        } else {
            lastUpdate = Date.now();
        }

        // Blink
        if (globalTimeMs < 10000 && globalTimeMs > 0) globalTimeDisplay.classList.add('blink-critical');
        else globalTimeDisplay.classList.remove('blink-critical');
        
        // Editable
        if (isGlobalPaused) globalTimeDisplay.classList.add('editable');
        else globalTimeDisplay.classList.remove('editable');

        // Robots Visual Update
        if(!isGlobalPaused) {
             robots.forEach(r => {
                if (r.running && r.time_left > 0) {
                    // Hier nur Visuals, Server sync kommt eh
                }
            });
        }
    }, 100);

    // --- WEBSOCKET ---
    function initWebSocket() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        socket = new WebSocket(protocol + '//' + window.location.hostname + '/ws');
        
        socket.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                lastUpdate = Date.now();

                if (typeof data.global_paused !== 'undefined') {
                    isGlobalPaused = data.global_paused;
                    globalPlayBtn.innerHTML = isGlobalPaused ? ICON_PLAY : ICON_PAUSE;
                    globalResetBtn.disabled = !isGlobalPaused;
                    updateButtonsState();
                }
                
                if (typeof data.global_time !== 'undefined') {
                    globalTimeMs = data.global_time;
                    globalTimeDisplay.textContent = formatTime(globalTimeMs / 1000);
                }

                if (data.robots) {
                    robots = data.robots;
                    if (!viewToggle.checked) renderRobots(robots);
                }

                if (data.leaderboard) {
                    leaderboardData = data.leaderboard;
                    if (viewToggle.checked) renderLeaderboard();
                }

            } catch (e) { console.error(e); }
        };
        socket.onclose = () => setTimeout(initWebSocket, 2000);
    }

    function formatTime(sec) {
        if(sec < 0) sec = 0;
        const m = Math.floor(sec / 60).toString().padStart(2, '0');
        const s = (Math.floor(sec) % 60).toString().padStart(2, '0');
        return `${m}:${s}`;
    }

    function updateButtonsState() {
        const btns = document.querySelectorAll('.robot-controls .btn-play-small');
        btns.forEach(btn => btn.disabled = isGlobalPaused);
    }

    function renderRobots(list) {
        const currentIds = list.map(r => r.id);
        Array.from(robotGrid.children).forEach(child => {
            if (!currentIds.includes(parseInt(child.dataset.id))) child.remove();
        });

        if(list.length === 0) {
            robotGrid.innerHTML = '<div style="text-align: center; color: #aaa; padding: 2rem;">Warte auf Verbindung...</div>';
            return;
        }

        list.forEach(rob => {
            let card = robotGrid.querySelector(`.robot-card[data-id="${rob.id}"]`);
            const statusClass = rob.online ? 'online' : 'offline';
            const pct = Math.min((rob.pieces / 36) * 100, 100);
            const btnIcon = rob.running ? ICON_PAUSE_SMALL : ICON_PLAY_SMALL;
            
            if (!card) {
                card = document.createElement('div');
                card.className = 'robot-card';
                card.dataset.id = rob.id;
                card.innerHTML = `
                    <div class="robot-header">
                        <div class="status-dot-wrapper">
                            <div class="status-dot ${statusClass}"></div>
                            <div class="status-tooltip">${rob.origin || "Unbekannt"}</div>
                        </div>
                        <button class="icon-btn locate-icon" onclick="locateRobot(${rob.id})">
                             <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 10c0 7-9 13-9 13s-9-6-9-13a9 9 0 0 1 18 0z"></path><circle cx="12" cy="10" r="3"></circle></svg>
                        </button>
                        <input type="text" class="robot-name-input" value="${rob.name}" placeholder="Team Name" onchange="updateName(${rob.id}, this.value)">
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
                robotGrid.appendChild(card);
            } else {
                card.querySelector('.status-dot').className = `status-dot ${statusClass}`;
                card.querySelector('.status-tooltip').textContent = rob.origin || "Unknown";
                const inp = card.querySelector('.robot-name-input');
                if (document.activeElement !== inp) inp.value = rob.name;
                
                const btn = card.querySelector('.btn-play-small');
                if(btn.innerHTML !== btnIcon) btn.innerHTML = btnIcon;
                // Important: Set Disabled property here every update
                btn.disabled = isGlobalPaused; 
                
                card.querySelector('.robot-time').textContent = formatTime(rob.time_left / 1000);
                card.querySelector('.bar').style.width = `${pct}%`;
                card.querySelector('.progress-text').textContent = `${rob.pieces} / 36`;
            }
        });
        updateButtonsState(); // Double check
    }

    function renderLeaderboard() {
        leaderboardList.innerHTML = "";
        if(leaderboardData.length === 0) {
            leaderboardList.innerHTML = "<div style='text-align:center; padding:2rem; color:#888;'>Keine Einträge</div>";
            return;
        }
        let displayData = [];
        if (filterMeanCheckbox.checked) {
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
        leaderboardList.appendChild(ul);
    }

    // ACTIONS
    window.locateRobot = (id) => socket.send(JSON.stringify({cmd: "locate", id}));
    window.updateName = (id, val) => socket.send(JSON.stringify({cmd: "set_name", id, val}));
    window.toggleRobot = (id) => socket.send(JSON.stringify({cmd: "toggle_robot", id}));
    window.adjScore = (id, val) => socket.send(JSON.stringify({cmd: "adj_score", id, val}));
    
    window.tryOpenGlobalTimeModal = function() {
        if(isGlobalPaused) timeModal.classList.add('active');
        else alert("Zeit kann nur im pausierten Zustand geändert werden.");
    };
    modalCancelBtn.onclick = () => timeModal.classList.remove('active');
    modalSaveBtn.onclick = () => {
        const m = parseInt(inputMin.value)||0;
        const s = parseInt(inputSec.value)||0;
        const total = m*60 + s;
        socket.send(JSON.stringify({cmd: "set_all_time", val: total}));
        timeModal.classList.remove('active');
    };

    globalPlayBtn.onclick = () => socket.send(JSON.stringify({cmd: "toggle_global"}));
    
    globalResetBtn.onclick = () => resetModal.classList.add('active');
    
    btnResetSave.onclick = () => {
        // Boolean true, not string "true"
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