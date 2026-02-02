// Icons
const ICON_EDIT = '<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"></path><path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"></path></svg>';
const ICON_TRASH = '<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"></polyline><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path></svg>';
const ICON_LOCK = '<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 10 0v4"></path></svg>';
const ICON_UNLOCK = '<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 9.9-1"></path></svg>';
const ICON_CHECK = '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><polyline points="20 6 9 17 4 12"></polyline></svg>';

let savedSSID = "";
let connectedSSID = "";
let isConnected = false;

// Einzige Konfig-Variable: Geräte-ID (number) oder nicht gesetzt (null => Auto)
let deviceId = null;

async function loadMdnsId() {
    const input = document.getElementById('mdnsIdInput');
    if (!input) return;

    try {
        const resp = await fetch('/mdns');
        if (!resp.ok) return;
        const data = await resp.json();

        if (data && typeof data.id === 'number' && Number.isFinite(data.id)) {
            const v = Math.trunc(data.id);
            deviceId = (v >= 1 && v <= 99) ? v : null;
        } else if (data && typeof data.id === 'string') {
            // Kompatibilität: alte Firmware konnte String liefern
            const s = data.id.trim();
            if (/^\d+$/.test(s)) {
                const v = parseInt(s, 10);
                deviceId = (v >= 1 && v <= 99) ? v : null;
            } else {
                deviceId = null;
            }
        } else {
            deviceId = null;
        }

        input.value = (deviceId === null) ? '' : String(deviceId);
    } catch (e) {
        // ignore
    }
}

async function saveMdnsId() {
    const input = document.getElementById('mdnsIdInput');
    const btn = document.getElementById('saveMdnsBtn');
    if (!input) return;

    const raw = (input.value || '').trim();
    if (raw === '') {
        deviceId = null;
    } else {
        if (!/^\d+$/.test(raw)) {
            alert('Bitte eine Zahl von 1 bis 99 eingeben (oder leer lassen).');
            return;
        }
        const v = parseInt(raw, 10);
        if (!(v >= 1 && v <= 99)) {
            alert('Bitte eine Zahl von 1 bis 99 eingeben (oder leer lassen).');
            return;
        }
        deviceId = v;
    }

    if (btn) {
        btn.disabled = true;
        btn.textContent = '...';
    }

    try {
        const resp = await fetch('/mdns', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ id: deviceId })
        });

        if (!resp.ok) {
            const msg = await resp.text();
            alert(msg || 'Fehler beim Speichern');
            return;
        }

        alert('Geräte-ID gespeichert. Wirkt beim nächsten Boot.');
        await loadMdnsId();
    } catch (e) {
        alert('Fehler beim Speichern');
    } finally {
        if (btn) {
            btn.disabled = false;
            btn.textContent = 'Speichern';
        }
    }
}

document.addEventListener('DOMContentLoaded', () => {
    // Theme Logic
    const toggle = document.getElementById('themeToggle');
    const savedTheme = localStorage.getItem('jenga_theme');
    if (savedTheme === 'dark') document.body.classList.add('dark');

    if (toggle) {
        toggle.addEventListener('click', () => { 
            document.body.classList.toggle('dark'); 
            const isDark = document.body.classList.contains('dark');
            localStorage.setItem('jenga_theme', isDark ? 'dark' : 'light');
        });
    }

    loadMdnsId();
    fetchNetworks();
});

async function fetchNetworks() {
    const list = document.getElementById('networkList');
    const refreshBtn = document.getElementById('refreshBtn');
    
    if (!list) return;

    list.innerHTML = '<div class="loading">Suche Netzwerke...</div>';
    if(refreshBtn) refreshBtn.disabled = true;

    try {
        // 1. Status laden (Connected & Saved)
        try {
            const statusResp = await fetch('/status');
            if (statusResp.ok) {
                const statusData = await statusResp.json();
                console.log("Status Data:", statusData); // Debugging
                savedSSID = statusData.saved_ssid || "";
                connectedSSID = statusData.connected_ssid || "";
                isConnected = statusData.connected;
            }
        } catch (e) { console.log("Status fetch error", e); }

        // 2. Scan
        const response = await fetch('/scan');
        if (!response.ok) throw new Error("Scan fehlgeschlagen");
        
        const networks = await response.json();
        list.innerHTML = '';

        // 3. Sortieren: Connected > Saved > RSSI
        networks.sort((a, b) => {
            const aIsConn = (a.ssid === connectedSSID && isConnected);
            const bIsConn = (b.ssid === connectedSSID && isConnected);
            if (aIsConn && !bIsConn) return -1;
            if (!aIsConn && bIsConn) return 1;

            const aIsSaved = (a.ssid === savedSSID);
            const bIsSaved = (b.ssid === savedSSID);
            if (aIsSaved && !bIsSaved) return -1;
            if (!aIsSaved && bIsSaved) return 1;
            
            return b.rssi - a.rssi;
        });

        if (networks.length === 0) {
            list.innerHTML = '<div class="loading">Keine Netzwerke gefunden.</div>';
        } else {
            networks.forEach(net => {
                const isSaved = (net.ssid === savedSSID && savedSSID !== "");
                const isConn = (net.ssid === connectedSSID && isConnected);
                
                const item = document.createElement('div');
                item.className = 'wifi-item';
                if (isConn) item.classList.add('connected');
                
                const safeSSID = net.ssid.replace(/'/g, "\\'");
                
                item.onclick = () => openModal(net.ssid);

                let statusBadges = '';
                if (isConn) statusBadges += `<span class="badge badge-success">${ICON_CHECK} Verbunden</span>`;
                else if (isSaved) statusBadges += `<span class="badge badge-info">Gespeichert</span>`;

                let html = `
                    <div class="wifi-info">
                        <div class="wifi-header">
                            <span class="wifi-ssid">${net.ssid}</span>
                            ${statusBadges}
                        </div>
                        <div class="wifi-meta">
                            Signal: ${net.rssi} dBm 
                            <span class="wifi-auth">${net.auth > 0 ? ICON_LOCK : ICON_UNLOCK}</span>
                        </div>
                    </div>
                `;

                if (isSaved || isConn) {
                    html += `
                    <div class="wifi-actions">
                        <button type="button" class="btn-icon" onclick="event.stopPropagation(); openModal('${safeSSID}')" title="Bearbeiten">
                            ${ICON_EDIT}
                        </button>
                        <button type="button" class="btn-icon delete" onclick="event.stopPropagation(); forgetNetwork('${safeSSID}')" title="Vergessen">
                            ${ICON_TRASH}
                        </button>
                    </div>`;
                }
                item.innerHTML = html;
                list.appendChild(item);
            });
        }

    } catch (error) {
        console.error('Error:', error);
        list.innerHTML = '<div class="loading" style="color:#ef4444">Fehler beim Laden.</div>';
    } finally {
        if(refreshBtn) refreshBtn.disabled = false;
    }
}

async function forgetNetwork(ssid) {
    showConfirm(`Netzwerk "${ssid}" wirklich vergessen?`, async () => {
        try {
            const resp = await fetch('/forget', { method: 'POST' });
            if (resp.ok) {
                // Reset local state
                if (savedSSID === ssid) savedSSID = "";
                fetchNetworks(); 
            } else {
                alert("Fehler beim Löschen.");
            }
        } catch (e) { 
            console.error(e);
        }
    });
}

function openModal(ssid) {
    const modal = document.getElementById('wifiModal');
    const ssidInput = document.getElementById('ssidInput');
    const passInput = document.getElementById('passwordInput');
    
    if(modal && ssidInput) {
        ssidInput.value = ssid;
        passInput.value = '';
        modal.classList.add('active'); 
        setTimeout(() => passInput.focus(), 100);
    }
}

function closeModal() {
    const modal = document.getElementById('wifiModal');
    if(modal) modal.classList.remove('active');
}

// Generic Confirm Modal
function showConfirm(message, onConfirm) {
    const modal = document.getElementById('confirmModal');
    const msgEl = document.getElementById('confirmMessage');
    const okBtn = document.getElementById('confirmOkBtn');
    const cancelBtn = document.getElementById('confirmCancelBtn');

    if (!modal) {
        if (confirm(message)) onConfirm();
        return;
    }

    msgEl.textContent = message;
    
    // Clean up old listeners
    const newOk = okBtn.cloneNode(true);
    const newCancel = cancelBtn.cloneNode(true);
    okBtn.parentNode.replaceChild(newOk, okBtn);
    cancelBtn.parentNode.replaceChild(newCancel, cancelBtn);

    newOk.addEventListener('click', () => {
        modal.classList.remove('active');
        onConfirm();
    });

    newCancel.addEventListener('click', () => {
        modal.classList.remove('active');
    });

    modal.classList.add('active');
}

function restartESP() {
    showConfirm("ESP32 wirklich neustarten?", () => {
        fetch('/restart', { method: 'POST' })
            .then(() => {
                alert("Neustart wird durchgeführt... Seite wird neu geladen.");
                setTimeout(() => location.reload(), 5000);
            })
            .catch(() => alert("Fehler beim Neustart"));
    });
}

window.onclick = function(event) {
    const wifiModal = document.getElementById('wifiModal');
    const confirmModal = document.getElementById('confirmModal');
    if (event.target == wifiModal) wifiModal.classList.remove('active');
    if (event.target == confirmModal) confirmModal.classList.remove('active');
}

function saveWifi() {
    const ssid = document.getElementById('ssidInput').value;
    const pass = document.getElementById('passwordInput').value;
    const btn = document.querySelector('.modal-actions .btn-primary');
    
    const originalText = btn.textContent;
    btn.textContent = "Verbinde...";
    btn.disabled = true;
    
    fetch('/save', { 
        method: 'POST', 
        headers: {'Content-Type': 'application/json'}, 
        body: JSON.stringify({ssid, password: pass}) 
    })
    .then((response) => {
        if(response.ok) {
            closeModal();
            // Wait a bit for connection attempt then refresh
            const list = document.getElementById('networkList');
            list.innerHTML = '<div class="loading">Verbinde mit ' + ssid + '...</div>';
            setTimeout(fetchNetworks, 4000);
        } else {
            throw new Error("Server Error");
        }
    })
    .catch(() => {
        alert('Fehler beim Speichern');
    })
    .finally(() => {
        btn.textContent = originalText;
        btn.disabled = false;
    });
}




