document.addEventListener('DOMContentLoaded', () => {
    
    // --- STATE ---
    let last_seen = Date.now(); // NEU: Für Connection Watchdog
    let gameState = {
        time: 0,
        maxTime: 0,
        mode: 'countdown', 
        isPlaying: false,
        moves: 0,
        leaderboard: []
    };

    let isDark = false;

    // --- DOM ELEMENTS ---
    const mainPage = document.getElementById('mainPage');
    // REMOVED CONFIG ELEMENTS
    const themeToggle = document.getElementById('themeToggle');
    // REMOVED CONFIG THEME TOGGLE
    
    const modeToggle = document.getElementById('modeToggle'); 
    const timeDisplay = document.getElementById('timeDisplay');
    const resetBtn = document.getElementById('resetBtn'); 
    const playPauseBtn = document.getElementById('playPauseBtn');
    const playIcon = document.getElementById('playIcon');
    const pauseIcon = document.getElementById('pauseIcon');
    const progressFill = document.getElementById('progressFill');
    
    const counterValue = document.getElementById('counterValue');
    const incrementBtn = document.getElementById('incrementBtn');
    const decrementBtn = document.getElementById('decrementBtn');

    // NEU: Leaderboard Inputs
    const teamNameInput = document.getElementById('teamNameInput');
    const towerFellBtn = document.getElementById('towerFellBtn');
    const towerFellIcon = document.getElementById('towerFellIcon'); 
    let isTowerFell = false;

    // SVGs für Turm Status
    const iconStanding = `<svg width="24" height="24" viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg"><path fill="currentColor" d="M20 85 h60 v-10 h-60 z M20 73 h18 v-10 h-18 z M41 73 h18 v-10 h-18 z M62 73 h18 v-10 h-18 z M20 61 h60 v-10 h-60 z M20 49 h18 v-10 h-18 z M41 49 h18 v-10 h-18 z M62 49 h18 v-10 h-18 z M20 37 h60 v-10 h-60 z M20 25 h18 v-10 h-18 z M41 25 h18 v-10 h-18 z M62 25 h18 v-10 h-18 z"/></svg>`;
    const iconFallen = `<svg width="24" height="24" viewBox="0 0 100 100" fill="currentColor" xmlns="http://www.w3.org/2000/svg"><path d="M10 90 h30 v-10 h-30 z M45 90 l15 -30 l9 4 l-15 30 z M75 90 h20 v-10 h-20 z M55 55 l25 8 l-3 9 l-25 -8 z M20 60 l20 -5 l3 9 l-20 5 z"/></svg>`;

    const timeModal = document.getElementById('timeModal');
    const inputMin = document.getElementById('inputMin');
    const inputSec = document.getElementById('inputSec');
    const modalSaveBtn = document.getElementById('modalSaveBtn');
    const modalCancelBtn = document.getElementById('modalCancelBtn');

    // LOCATE BTN
    const locateBtn = document.getElementById('locateBtn');
    if (locateBtn) {
        locateBtn.addEventListener('click', () => {
             // Disable for 3 seconds
             locateBtn.disabled = true;
             sendCommand('locate_device');
             setTimeout(() => {
                 locateBtn.disabled = false;
             }, 3000);
        });
    }

    // --- THEME INIT ---
    const savedTheme = localStorage.getItem('theme');
    if (savedTheme === 'dark') {
        isDark = true;
        document.body.classList.add('dark');
    }

    // --- NAVIGATION & THEME ---
    
    // REMOVED switchPage function (Config page removed)

    function toggleTheme() {
        isDark = !isDark;
        document.body.classList.toggle('dark', isDark);
        localStorage.setItem('theme', isDark ? 'dark' : 'light');
    }

    // REMOVED settingsBtn / backBtn listeners
    themeToggle.addEventListener('click', toggleTheme);
    // REMOVED themeToggleConfig listener

    // --- GAME LOGIC ---
    // Tower Fell Logic - Consolidated
    
    // Remove the old duplicate updateTowerFellUI and listener if they exist
    // The previous code had two definitions of updateTowerFellUI and two listeners. 
    // We are replacing the FIRST block here, effective removing it or redefining it properly.
    
    // Actually, I will just remove this block entirely as the better logic is further down.
    // Or better, I will keep declaration here but empty, to avoid reference errors if it's called before definition?
    // JS functions are hoisted if function decoration. But if const, no.
    // The other definition was a function declaration: function updateTowerFellUI() {}
    // So it overwrites?
    // In JS: Last function declaration wins.
    // But the listeners are attached sequentially.
    // So both listeners run.
    
    // I will replace lines 73-88 with NOTHING (or verify they are gone).
    // Ah, wait. Lines 73-82 were:
    /*
    function updateTowerFellUI() {
        if (isTowerFell) {
            towerFellBtn.classList.add('btn-primary');
            ...
    */
    // And lines 503-524 were the second definition.
    // And lines 86-89 were the FIRST listener.
    // And lines 527-535 were the SECOND listener.
    
    // ACTION: I will remove the FIRST Listener and the FIRST Function (or modify it to use the new logic if I want code locality).
    // It's cleaner to remove the top block and keep the bottom block, BUT the bottom block is inside `if (towerFellBtn)`.
    // The top block assumes `towerFellBtn` exists.
    
    // I will remove the top block.


    function formatTime(seconds) {
        const m = Math.floor(seconds / 60).toString().padStart(2, '0');
        const s = (seconds % 60).toString().padStart(2, '0');
        return `${m}:${s}`;
    }

    function updateUI() {
        timeDisplay.textContent = formatTime(gameState.time);
        
        // PAUSE GAME IF TOWER FELL
        if (isTowerFell && gameState.isPlaying) {
             sendCommand('toggle_pause');
        }
        // Blinken
        if (gameState.mode === 'countdown' && gameState.time <= 10 && gameState.time > 0 && gameState.isPlaying) {
            timeDisplay.classList.add('blink-red');
        } else {
            timeDisplay.classList.remove('blink-red');
        }

        // Editierbarkeit anzeigen
        if (gameState.mode === 'countdown' && !gameState.isPlaying) {
            timeDisplay.classList.add('editable');
            timeDisplay.title = "Klicken zum Bearbeiten der Startzeit";
        } else {
            timeDisplay.classList.remove('editable');
            timeDisplay.title = "";
        }

        // Progress Bar
        if (gameState.mode === 'countdown' && gameState.maxTime > 0) {
            const percentage = (gameState.time / gameState.maxTime) * 100;
            progressFill.style.width = `${percentage}%`;
        } else {
            progressFill.style.width = '100%';
        }

        // Buttons Status
        if (gameState.isPlaying) {
            playIcon.classList.add('hidden');
            pauseIcon.classList.remove('hidden');
            if (resetBtn) resetBtn.disabled = true;
            if (modeToggle) modeToggle.disabled = true;
            
            // Play/Pause Button ist aktiv (klickbar)
            playPauseBtn.disabled = false; 
            
        } else {
            playIcon.classList.remove('hidden');
            pauseIcon.classList.add('hidden');
            
            // NEU: Reset Button Logic (Nur aktiv, wenn Zeit lief oder Züge gemacht wurden)
            if (resetBtn) {
                let hasProgress = false;
                if (gameState.moves > 0) hasProgress = true;
                else {
                    if (gameState.mode === 'countdown') {
                        // Progress means time < maxTime
                        if (gameState.maxTime > 0 && gameState.time < gameState.maxTime) hasProgress = true;
                    } else { // countup
                        if (gameState.time > 0) hasProgress = true;
                    }
                }
                resetBtn.disabled = !hasProgress;
            }

            if (modeToggle) modeToggle.disabled = false;

            // --- NEU: Play Button deaktivieren wenn Zeit abgelaufen ---
            // Wird jetzt von updatePlayButtonState handled
            updatePlayButtonState();
        }

        counterValue.textContent = gameState.moves;
        decrementBtn.disabled = gameState.moves <= 0;

        // NEU: Tower Fell Button Logic
        if (towerFellBtn) {
            let hasTimeRun = false;
            // Toleranz: Zeit muss sich vom Startwert unterscheiden
            if (gameState.mode === 'countdown') {
                 // Check if time is effectively smaller than maxTime
                 if (gameState.maxTime > 0 && gameState.time < gameState.maxTime) hasTimeRun = true;
            } else { // countup
                 if (gameState.time > 0) hasTimeRun = true;
            }
            
            // Button deaktivieren, solange keine Zeit gelaufen ist.
            // Ausnahme: Wenn Turm bereits gefallen ist (damit man es korrigieren kann),
            // obwohl das theoretisch nicht passieren sollte, wenn logic strikt ist.
            if (isTowerFell) {
                 towerFellBtn.disabled = false;
            } else {
                 towerFellBtn.disabled = !hasTimeRun;
            }
        }

        if (modeToggle) {
            const shouldBeChecked = (gameState.mode === 'countup');
            if (modeToggle.checked !== shouldBeChecked) {
                modeToggle.checked = shouldBeChecked;
            }
        }
    }

    // --- API / WEBSOCKET ---
    let socket;

    // --- LEADERBOARD LOGIC (Fullscreen, Filter, Edit) ---
    const leaderboardContainer = document.getElementById('leaderboardContainer');
    const fullscreenBtn = document.getElementById('fullscreenBtn');
    const filterBtn = document.getElementById('filterBtn');
    
    // State
    let isFullscreen = false;
    let filterOnlyBest = false;
    let filterTowerStanding = false;
    let currentEditEntryIndex = null; // Index in the currently displayed filtered list? Or ID?
    // Using ID is better if we have it. Backend sends entry_id.
    
    // Icons
    const iconMinimize = `<svg class="icon" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="6" x2="6" y2="18"></line><line x1="6" y1="6" x2="18" y2="18"></line></svg>`; // X Icon
    const iconMaximize = `<svg class="icon" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3"></path></svg>`;
    const iconEdit = `<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"></path><path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"></path></svg>`;

    // --- FULLSCREEN TOGGLE ---
    fullscreenBtn.addEventListener('click', () => {
        isFullscreen = !isFullscreen;
        leaderboardContainer.classList.toggle('fullscreen', isFullscreen);
        fullscreenBtn.innerHTML = isFullscreen ? iconMinimize : iconMaximize;
        
        // Show/Hide Footer
        document.getElementById('leaderboardFooter').classList.toggle('hidden', !isFullscreen);

        // Re-render to show/hide edit icons
        renderLeaderboard(gameState.leaderboard);
    });

    // --- FILTER MODAL ---
    const filterModal = document.getElementById('filterModal');
    const filterBestToggle = document.getElementById('filterBestToggle');
    const filterTowerToggle = document.getElementById('filterTowerToggle');
    const filterCloseBtn = document.getElementById('filterCloseBtn');

    filterBtn.addEventListener('click', () => {
        filterModal.classList.add('active');
    });

    filterCloseBtn.addEventListener('click', () => {
        filterModal.classList.remove('active');
    });

    // Apply Filter Logic
    function getFilteredLeaderboard(entries) {
        if (!entries) return [];
        let filtered = [...entries];

        // Filter 1: Tower Standing (Only keep entries where fell == false)
        if (filterTowerStanding) {
            filtered = filtered.filter(e => !e.fell);
        }

        // Filter 2: Best per Team
        if (filterOnlyBest) {
            const bestMap = new Map();
            filtered.forEach(entry => {
                const team = entry.team || "Unknown";
                // Criteria: Mode specific?
                // Assuming standard logic: More moves is better. If moves equal, less time is better (in countdown: more remaining time? No, backend sends duration).
                // Backend sorted it already. So the first occurrence is the best?
                // Actually backend sort: Moves DESC, Time ASC. So yes, first occurrence is best.
                if (!bestMap.has(team)) {
                    bestMap.set(team, entry);
                }
            });
            filtered = Array.from(bestMap.values());
            // Re-sort because Map iteration order is insertion order, which is correct here as input was sorted.
        }

        return filtered;
    }

    // Toggle Handlers
    filterBestToggle.addEventListener('change', (e) => {
        filterOnlyBest = e.target.checked;
        renderLeaderboard(gameState.leaderboard);
    });

    filterTowerToggle.addEventListener('change', (e) => {
        filterTowerStanding = e.target.checked;
        renderLeaderboard(gameState.leaderboard);
    });

    // --- PASSWORD & EDIT LOGIC ---
    const passwordModal = document.getElementById('passwordModal');
    const passwordInput = document.getElementById('passwordInput');
    const pwConfirmBtn = document.getElementById('pwConfirmBtn');
    const pwCancelBtn = document.getElementById('pwCancelBtn');
    
    let pendingAction = null; // 'edit' or 'deleteAll'
    let pendingEntryId = null;

    function requestPassword(action, entryId = null) {
        pendingAction = action;
        pendingEntryId = entryId;
        passwordInput.value = '';
        passwordModal.classList.add('active');
        passwordInput.focus();
    }

    function closePasswordModal() {
        passwordModal.classList.remove('active');
        pendingAction = null;
        pendingEntryId = null;
    }

    pwCancelBtn.addEventListener('click', closePasswordModal);

    pwConfirmBtn.addEventListener('click', () => {
        if (passwordInput.value === 'Weihnachtsbaum') {
            // Save state before closing (which clears globals)
            const action = pendingAction;
            const id = pendingEntryId;
            
            closePasswordModal();
            
            if (action === 'edit') {
                openEditModal(id);
            } else if (action === 'deleteAll') {
                sendCommand('clear_leaderboard');
            }
        } else {
            alert('Falsches Passwort!');
            passwordInput.value = '';
            passwordInput.focus();
        }
    });
    
    // --- EDIT MODAL ---
    const editEntryModal = document.getElementById('editEntryModal');
    const editTeamName = document.getElementById('editTeamName');
    const editMoves = document.getElementById('editMoves');
    const editTimeSec = document.getElementById('editTimeSec');
    const editTowerFellToggle = document.getElementById('editTowerFellToggle');
    const editSaveBtn = document.getElementById('editSaveBtn');
    const editCancelBtn = document.getElementById('editCancelBtn');
    const deleteEntryBtn = document.getElementById('deleteEntryBtn');
    
    let editingId = null;

    function openEditModal(id) {
        console.log("openEditModal called with ID:", id); // DEBUG
        console.log("Current Leaderboard:", gameState.leaderboard); // DEBUG

        // Robust matching (String vs Number)
        const entry = gameState.leaderboard.find(e => String(e.id) === String(id) || String(e.entry_id) === String(id)); 
        
        console.log("Found Entry:", entry); // DEBUG

        if (!entry) return;

        editingId = entry.entry_id || entry.id; // Prefer entry_id (number) for backend API
        editTeamName.value = entry.team;
        editMoves.value = entry.moves;
        editTimeSec.value = Math.floor(entry.time / 1000); // Display in seconds? Or raw ms? Let's do seconds.
        editTowerFellToggle.checked = entry.fell;
        
        editEntryModal.classList.add('active');
    }

    function closeEditModal() {
        editEntryModal.classList.remove('active');
        editingId = null;
    }

    editCancelBtn.addEventListener('click', closeEditModal);

    editSaveBtn.addEventListener('click', () => {
        if (editingId === null) return;
        
        const payload = {
            cmd: 'update_entry',
            id: editingId,
            team: editTeamName.value,
            moves: parseInt(editMoves.value),
            time: parseInt(editTimeSec.value) * 1000,
            fell: editTowerFellToggle.checked
        };
        socket.send(JSON.stringify(payload));
        closeEditModal();
    });

    deleteEntryBtn.addEventListener('click', () => {
         if (editingId === null) return;
         if(confirm("Diesen Eintrag wirklich löschen?")) {
             const payload = {
                 cmd: 'delete_entry',
                 id: editingId
             };
             socket.send(JSON.stringify(payload));
             closeEditModal();
         }
    });

    // Delete All
    document.getElementById('deleteAllBtn').addEventListener('click', () => {
        requestPassword('deleteAll');
    });

    // --- RENDER LEADERBOARD (UPDATED) ---
    function renderLeaderboard(entries) {
        // Use filtered list
        const filteredEntries = getFilteredLeaderboard(entries);

        leaderboardList.innerHTML = '';
        if (!filteredEntries || filteredEntries.length === 0) {
            leaderboardList.innerHTML = '<div class="leaderboard-item"><div class="leaderboard-info" style="text-align: center; width: 100%;">Keine Einträge (Filter aktiv?)</div></div>';
            return;
        }

        filteredEntries.forEach((entry, index) => {
            const item = document.createElement('div');
            item.className = 'leaderboard-item';
            
            // Icon für Turm Status
            const statusIcon = entry.fell ? iconFallen : iconStanding;
            // Zeit formatieren
            const m = Math.floor(entry.time / 60000).toString().padStart(2, '0');
            const s = Math.floor((entry.time % 60000) / 1000).toString().padStart(2, '0');
            const timeStr = `${m}:${s}`;
            
            // Use entry ID (backend provided)
            const entryId = entry.entry_id || entry.id || index; // Fallback

            let html = `
                <div class="leaderboard-rank">#${index + 1}</div>
                <div class="leaderboard-info">
                    <span class="team-name">${entry.team}</span>
                    <span class="team-stats">${entry.moves} Züge | ${timeStr} | ${statusIcon}</span>
                </div>
            `;
            
            // Add Edit Icon ONLY if Fullscreen
            if (isFullscreen) {
                html += `<button class="edit-icon-btn" data-id="${entryId}" aria-label="Edit">${iconEdit}</button>`;
            }

            item.innerHTML = html;
            leaderboardList.appendChild(item);
        });

        // Add Listeners to new buttons
        if (isFullscreen) {
            document.querySelectorAll('.edit-icon-btn').forEach(btn => {
                btn.addEventListener('click', (e) => {
                    // Prevent bubbling?
                    e.stopPropagation();
                    const id = btn.dataset.id; // Use string for robustness
                    requestPassword('edit', id);
                });
            });
        }
    }

    function initWebSocket() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = protocol + '//' + window.location.hostname + '/ws';
        
        // Ensure only one socket is attempted at a time
        if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) {
             return;
        }

        socket = new WebSocket(wsUrl);
        
        // Select Banner Element (Added dynamically in HTML via edit)
        const banner = document.getElementById('connectionBanner');

        socket.onopen = function() { 
            console.log('WebSocket connected'); 
            if (banner) banner.classList.remove('visible');
        };

        socket.onmessage = function(event) {
            try {
                const data = JSON.parse(event.data);
                
                // NEU: Heartbeat Logic
                if (typeof data.server_timestamp !== 'undefined') {
                    last_seen = Date.now();
                }

                if (typeof data.mode !== 'undefined') gameState.mode = (data.mode === 1) ? 'countup' : 'countdown';
                
                let receivedTime = (gameState.mode === 'countdown') ? data.time_countdown : data.time_countup;
                if (typeof receivedTime === 'number' && !isNaN(receivedTime)) gameState.time = Math.floor(receivedTime / 1000);
                
                if (typeof data.is_paused !== 'undefined') gameState.isPlaying = !data.is_paused;
                if (typeof data.piece_counter !== 'undefined') gameState.moves = data.piece_counter;
                if (typeof data.countdown_start !== 'undefined') gameState.maxTime = Math.floor(data.countdown_start / 1000);
                
                // NEU: Teamname syncen (wenn abweichend)
                if (typeof data.current_team !== 'undefined' && teamNameInput.value !== data.current_team) {
                    // Check if input is focused to avoid disrupting typing
                    if (document.activeElement !== teamNameInput) {
                        teamNameInput.value = data.current_team;
                        updatePlayButtonState();
                    }
                }

                // NEU: Leaderboard Update - Nur rendern wenn Daten sich geändert haben
                if (data.leaderboard) {
                   const newLbString = JSON.stringify(data.leaderboard);
                   // Prüfen ob globaler Speicher existiert
                   if (typeof window.lastLeaderboardJson === 'undefined') window.lastLeaderboardJson = "";
                   
                   if (newLbString !== window.lastLeaderboardJson || isFullscreen !== (window.lastIsFullscreen || false) || 
                       filterOnlyBest !== (window.lastFilterBest || false) || filterTowerStanding !== (window.lastFilterTower || false)) {
                       
                       console.log("Leaderboard updated! New Data:", data.leaderboard); // DEBUG
                       gameState.leaderboard = data.leaderboard;
                       renderLeaderboard(data.leaderboard);
                       
                       window.lastLeaderboardJson = newLbString;
                       window.lastIsFullscreen = isFullscreen;
                       window.lastFilterBest = filterOnlyBest;
                       window.lastFilterTower = filterTowerStanding;
                   }
                }

                updateUI();
            } catch (e) { console.error('Error parsing WS message', e); }
        };

        socket.onclose = function() { 
            console.warn('WebSocket disconnected. Reconnecting in 2s...');
            if (banner) banner.classList.add('visible');
            setTimeout(initWebSocket, 2000); 
        };
        
        socket.onerror = function(err) {
            console.error('WebSocket error:', err);
             // OnError usually precedes OnClose, so visible banner handled there or here
             if (banner) banner.classList.add('visible');
        };
    }

    async function sendCommand(command, value = null) {
        if (!socket || socket.readyState !== WebSocket.OPEN) return;
        let payload = {};
        switch (command) {
            case 'toggle_pause': payload = { cmd: 'toggle_pause' }; break;
            case 'set_mode': payload = { cmd: 'set_mode', val: (value === 'countup' ? 1 : 0) }; break;
            case 'adjust_moves': payload = { cmd: (value > 0 ? 'inc_moves' : 'dec_moves') }; break;
            case 'set_time': payload = { cmd: 'set_time', val: value * 1000 }; break;
            case 'reset_time': payload = { cmd: 'reset_time' }; break;
            case 'save_and_reset': 
                payload = { 
                    cmd: 'save_and_reset', 
                    team: value.team, 
                    fell: value.fell 
                }; 
                break;
            case 'set_team_name':
                payload = { cmd: 'set_team_name', team: value };
                break;
            case 'locate_device': payload = { cmd: 'locate_device' }; break;
            case 'clear_leaderboard': payload = { cmd: 'clear_leaderboard' }; break;
        }
        socket.send(JSON.stringify(payload));
    }

    // --- UPDATE TOWER STAT UI ---
    function updateTowerFellUI() {
        if (!towerFellBtn) return;
        
        if (isTowerFell) {
            towerFellBtn.style.color = '#FF8C00'; // Orange
            towerFellBtn.innerHTML = `<span style="display: flex; align-items: center; color: #FF8C00;">${iconFallen}</span>`;
        } else {
             towerFellBtn.style.color = ''; // Default
             towerFellBtn.innerHTML = `<span id="towerFellIcon" style="display: flex; align-items: center;">${iconStanding}</span>`;
        }
    }

    if (towerFellBtn) {
        towerFellBtn.addEventListener('click', () => {
             isTowerFell = !isTowerFell;
             updateTowerFellUI();
             // User Request: "Wenn der turm gefallen ist, soll das SPiel ebenfalls pausiert werden."
             if (isTowerFell && gameState.isPlaying) {
                 sendCommand('toggle_pause');
             }
        });
    }

    // --- INPUT VALIDATION ---
    function updatePlayButtonState() {
        // Play Button ist disabled, wenn:
        // 1. TeamName leer ist
        // 2. ODER (Countdown Modus UND Zeit <= 0)
        
        const hasTeamName = teamNameInput.value.trim().length > 0;
        const isCountdownFinished = (gameState.mode === 'countdown' && gameState.time <= 0);
        
        if (!hasTeamName || isCountdownFinished) {
            playPauseBtn.disabled = true;
            // Optional: Visueller Hinweis
            if (!hasTeamName) teamNameInput.style.borderColor = "red";
            else teamNameInput.style.borderColor = "";
        } else {
            playPauseBtn.disabled = false;
            teamNameInput.style.borderColor = "";
        }
    }

    // Event Listener für Team Name
    teamNameInput.addEventListener('input', () => {
        const name = teamNameInput.value.trim();
        updatePlayButtonState();
        // Name an ESP senden (damit Hardware-Button Bescheid weiß)
        sendCommand('set_team_name', name);
    });

    // Initial Prüfung
    updatePlayButtonState();

    // --- EVENT LISTENERS ---

    // 1. Standard Buttons
    if (resetBtn) {
        resetBtn.addEventListener('click', () => {
            if (!gameState.isPlaying) {
                // Senden mit Teamname und Status
                const teamName = teamNameInput.value || "Team " + (Math.floor(Math.random() * 1000));
                sendCommand('save_and_reset', { team: teamName, fell: isTowerFell });
                
                // Checkbox zurücksetzen (User Request: "Bei Reset soll die Checkbox ebenfalls zurückgesetzt werden")
                isTowerFell = false;
                updateTowerFellUI();
                
                // Teamname bleibt stehen (User Request: "der Teamname soll aber gespeichert bleiben")
            }
        });
    }

    modeToggle.addEventListener('change', (e) => {
        if (!gameState.isPlaying) {
            const newMode = e.target.checked ? 'countup' : 'countdown';
            sendCommand('set_mode', newMode);
            
            // Reset Tower Fell Status when switching modes
            if (isTowerFell) {
                isTowerFell = false;
                updateTowerFellUI();
            }
        }
    });

    playPauseBtn.addEventListener('click', () => sendCommand('toggle_pause'));
    incrementBtn.addEventListener('click', () => sendCommand('adjust_moves', 1));
    decrementBtn.addEventListener('click', () => {
        if (gameState.moves > 0) sendCommand('adjust_moves', -1);
    });

    // 2. Time Edit Modal Logic
    timeDisplay.addEventListener('click', () => {
        if (!gameState.isPlaying && gameState.mode === 'countdown') {
            const m = Math.floor(gameState.maxTime / 60);
            const s = gameState.maxTime % 60;
            inputMin.value = m.toString().padStart(2, '0');
            inputSec.value = s.toString().padStart(2, '0');
            timeModal.classList.add('active');
            setTimeout(() => { inputMin.focus(); inputMin.select(); }, 50);
        }
    });

    inputMin.addEventListener('input', () => {
        if (inputMin.value.length >= 2) { inputSec.focus(); inputSec.select(); }
    });

    function handleEnter(e) { if (e.key === 'Enter') saveTime(); }
    inputMin.addEventListener('keydown', handleEnter);
    inputSec.addEventListener('keydown', handleEnter);

    modalCancelBtn.addEventListener('click', () => timeModal.classList.remove('active'));
    modalSaveBtn.addEventListener('click', saveTime);

    function saveTime() {
        let m = parseInt(inputMin.value) || 0;
        let s = parseInt(inputSec.value) || 0;
        if (s > 59) s = 59;
        if (m < 0) m = 0; if (s < 0) s = 0;
        const totalSeconds = (m * 60) + s;
        if (totalSeconds > 0) {
            sendCommand('set_time', totalSeconds);
            timeModal.classList.remove('active');
        }
    }

    timeModal.addEventListener('click', (e) => {
        if (e.target === timeModal) timeModal.classList.remove('active');
    });

    // --- INITIALIZATION ---

    // NEU: Connection Watchdog (alle 500ms prüfen)
    setInterval(() => {
        const banner = document.getElementById('connectionBanner');
        if (!banner) return;

        const diff = Date.now() - last_seen;
        const isSocketOpen = socket && socket.readyState === WebSocket.OPEN;
        
        if (!isSocketOpen || diff > 3000) {
             // ROT: Verbindung verloren (> 3s oder Socket zu)
             banner.textContent = "Verbindung verloren... Versuche Reconnect";
             banner.classList.remove('unstable');
             banner.classList.add('visible');
        } else if (diff > 1000) {
             // GELB: Verbindung instabil (> 1s)
             banner.textContent = "Verbindung instabil...";
             banner.classList.add('unstable');
             banner.classList.add('visible');
        } else {
            // ALLES OK
            banner.classList.remove('visible');
            banner.classList.remove('unstable');
        }
    }, 500);

    updateUI();
    initWebSocket();
});