const safe = name => name.replace(/\.pwp$/i, "").replace(/[^a-z0-9_-]/gi, "-").slice(0, 40) || "library";
const storage = document.getElementById("storage-status");
const installable = Number(storage.dataset.installable);
const sizeText = bytes => bytes >= 1048576
    ? (bytes / 1048576).toFixed(1) + " MB"
    : Math.ceil(bytes / 1024) + " KB";
const transferText = (received, total) => total > 0
    ? "Downloading… " + Math.floor(received * 100 / total) + "% · " +
      Math.floor(received / 1024) + "/" + Math.floor(total / 1024) + " KB"
    : "Downloading… " + Math.floor(received / 1024) + " KB";

function showStatus(element, message) {
    element.textContent = message;
    element.setAttribute("role", "status");
}

async function install(blob, name) {
    const status = document.getElementById("upload-status");
    const progress = document.getElementById("upload-progress");
    if (blob.size > installable) {
        showStatus(status, "This pack is " + sizeText(blob.size) + ", but only " +
            sizeText(installable) + " is available.");
        return false;
    }
    progress.hidden = false;
    progress.max = 100;
    progress.value = 0;
    showStatus(status, "Installing pack…");
    return new Promise(resolve => {
        const request = new XMLHttpRequest();
        request.open("POST", "/api/packs/upload?name=" + encodeURIComponent(safe(name)));
        request.upload.onprogress = event => {
            if (event.lengthComputable) {
                const value = Math.round(event.loaded * 100 / event.total);
                progress.value = value;
                showStatus(status, "Installing pack… " + value + "%");
            }
        };
        request.onload = () => {
            let result = { ok: false, error: "PocketWiki could not install that pack." };
            try { result = JSON.parse(request.responseText); } catch (error) { /* keep the fallback */ }
            progress.hidden = true;
            if (result.ok) {
                showStatus(status, "Pack installed — " + result.articles + " articles.");
                setTimeout(() => location.reload(), 700);
            } else {
                showStatus(status, result.error || "PocketWiki could not install that pack.");
            }
            resolve(result.ok);
        };
        request.onerror = () => {
            progress.hidden = true;
            showStatus(status, "The connection was lost while installing. Check your library before trying again.");
            resolve(false);
        };
        request.send(blob);
    });
}

document.getElementById("pack-upload").onsubmit = event => {
    event.preventDefault();
    const file = document.getElementById("pack-file").files[0];
    if (file) install(file, file.name);
};

document.getElementById("url-upload").onsubmit = async event => {
    event.preventDefault();
    const url = document.getElementById("pack-url").value.trim();
    const status = document.getElementById("upload-status");
    await installFromDevice(url, safe(url.split("/").pop()), status,
        document.getElementById("upload-progress"));
};

const catalogList = document.getElementById("pack-catalog");
const catalogStatus = document.getElementById("catalog-status");
const catalogProgress = document.getElementById("catalog-progress");
/* Last uplink state from /api/wifi/status: null until it is known. */
let deviceUplink = null;
const catalogSync = document.getElementById("catalog-sync");
const syncedAt = document.getElementById("catalog-synced");
const installed = new Set([...document.querySelectorAll("[data-pack-name]")]
    .map(element => element.dataset.packName));
/* Libraries that ship with the device. They are always present, so the
 * catalogue marks them "Included" until an installed copy updates them. */
const builtin = new Set([...document.querySelectorAll("[data-builtin]")]
    .map(element => element.dataset.packName || "pocketwiki"));

/* Ticking packs and installing them in one go. The device takes one upload at a
 * time, so a selection is a queue; the free space it reported is the budget the
 * whole selection has to fit inside. */
const chosen = new Map();
const catalogActions = document.getElementById("catalog-actions");
const catalogCount = document.getElementById("catalog-count");
const catalogRoom = document.getElementById("catalog-room");
const catalogClear = document.getElementById("catalog-clear");
const catalogInstall = document.getElementById("catalog-install");
let rendered = [];
let installing = false;

async function installFromDevice(url, name, statusEl, progress = document.getElementById("catalog-progress"), label = "") {
    /* The label marks a pack's place in a batch on the lines that move; the
     * verdict lines below stand alone. */
    const setStatus = (message, prefix = label) => {
        statusEl.textContent = prefix + message;
        if (statusEl.setAttribute) statusEl.setAttribute("role", "status");
    };
    progress.hidden = false;
    progress.max = 100;
    progress.value = 0;
    setStatus("Starting download…");
    let result = null;
    const consume = line => {
        const match = /^P:(\d+):(-?\d+)$/.exec(line);
        if (match) {
            const received = Number(match[1]);
            const total = Number(match[2]);
            if (total > 0) {
                progress.max = total;
                progress.value = received;
            } else {
                progress.removeAttribute("value");
            }
            setStatus(transferText(received, total));
        } else if (line.startsWith("OK:")) {
            result = true;
            setStatus("Pack installed — " + line.slice(3) + " articles.", "");
        } else if (line.startsWith("ERR:")) {
            result = false;
            setStatus(line.slice(4) || "PocketWiki could not install that pack.", "");
        }
    };
    try {
        const response = await fetch("/api/packs/install?url=" + encodeURIComponent(url) +
            "&name=" + encodeURIComponent(name), { method: "POST", cache: "no-store" });
        if (!response.ok) {
            let message = "PocketWiki could not install that pack (HTTP " + response.status + ").";
            try {
                const error = await response.json();
                if (error.error) message = error.error;
            } catch (error) { /* keep the fallback */ }
            setStatus(message);
            return false;
        }
        const reader = response.body.getReader();
        const decoder = new TextDecoder();
        let pending = "";
        for (;;) {
            const { value, done } = await reader.read();
            pending += done ? decoder.decode() : decoder.decode(value, { stream: true });
            let end;
            while ((end = pending.indexOf("\n")) >= 0) {
                consume(pending.slice(0, end).trim());
                pending = pending.slice(end + 1);
            }
            if (done) break;
        }
        if (pending.trim()) consume(pending.trim());
        if (result === null) {
            setStatus("The connection ended before PocketWiki confirmed the install. Reload to check your library.");
        }
        return result === true;
    } catch (error) {
        if (result === null) {
            setStatus("The connection was lost while installing. Reload to check your library before trying again.");
        }
        return result === true;
    } finally {
        progress.hidden = true;
    }
}

function renderCatalog(catalog) {
    catalogList.replaceChildren();
    chosen.clear();
    rendered = catalog.packs;
    catalog.packs.forEach(pack => {
        const row = document.createElement("div");
        row.className = "catalog-item";

        const box = document.createElement("input");
        box.type = "checkbox";
        box.className = "catalog-select";
        box.setAttribute("aria-label", "Install " + pack.name);
        const room = pack.bytes <= installable;
        const fits = room && deviceUplink !== false;
        box.disabled = !fits;
        box.onchange = () => {
            if (box.checked) chosen.set(pack.id, pack);
            else chosen.delete(pack.id);
            syncSelection();
        };
        if (!fits) {
            box.title = deviceUplink === false
                ? "This device has no Wi-Fi uplink. Download the pack on this phone and install it with the file form below."
                : "This pack needs " + sizeText(pack.bytes) + "; " + sizeText(installable) + " is available.";
        }

        const copy = document.createElement("div");
        copy.className = "catalog-copy";
        const title = document.createElement("strong");
        title.textContent = pack.name;
        if (installed.has(pack.id)) {
            const badge = document.createElement("span");
            badge.className = "catalog-installed";
            badge.textContent = "Installed";
            title.append(" ", badge);
        } else if (builtin.has(pack.id)) {
            const badge = document.createElement("span");
            badge.className = "catalog-installed";
            badge.textContent = "Included";
            title.append(" ", badge);
        }
        const meta = document.createElement("span");
        meta.textContent = pack.articles + " articles · version " + pack.version + " · " +
            Math.ceil(pack.bytes / 1024) + " KB";
        const description = document.createElement("span");
        /* A tick that cannot be made needs its reason on screen, not in a
         * tooltip a touch screen never shows. The device having no uplink is
         * not this pack's fault, and the status line above already says so. */
        if (!room) {
            description.textContent = "Needs " + sizeText(pack.bytes) + "; only " +
                sizeText(installable) + " is available.";
            description.className = "short";
        } else {
            description.textContent = pack.description;
        }
        copy.append(title, meta, description);

        row.append(box, copy);
        catalogList.append(row);
    });
    syncSelection();
}

/* The bar is the only place the selection is spent, so it also carries the one
 * fact that can stop it: the whole selection has to fit the space the device
 * reported, not each pack on its own. */
function syncSelection() {
    const packs = [...chosen.values()];
    const bytes = packs.reduce((total, pack) => total + pack.bytes, 0);
    const fits = bytes <= installable;
    catalogActions.hidden = installing || packs.length === 0;
    if (installing) return;
    catalogCount.textContent = packs.length === 1
        ? "1 pack · " + sizeText(bytes)
        : packs.length + " packs · " + sizeText(bytes);
    catalogRoom.textContent = fits
        ? sizeText(installable) + " available"
        : sizeText(bytes - installable) + " more than the " + sizeText(installable) + " available";
    catalogRoom.classList.toggle("short", !fits);
    catalogInstall.disabled = !fits || deviceUplink === false;
    catalogInstall.textContent = packs.length === 1 ? "Install" : "Install " + packs.length;
}

/** Install everything ticked, in catalogue order. One download and one upload
 *  at a time: the device takes a single upload session and needs the free
 *  space the previous pack left, so a batch is a queue rather than a fan-out. */
async function installSelected() {
    const queue = rendered.filter(pack => chosen.has(pack.id));
    if (!queue.length || installing) return;
    installing = true;
    catalogClear.disabled = true;
    syncSelection();
    let done = 0;
    let ok = true;
    for (const pack of queue) {
        done++;
        ok = await installFromDevice(pack.url, pack.id, catalogStatus, catalogProgress,
            queue.length > 1 ? "Pack " + done + " of " + queue.length + " · " : "");
        if (!ok) break;
    }
    installing = false;
    catalogClear.disabled = false;
    if (ok) {
        chosen.clear();
        if (queue.length > 1) showStatus(catalogStatus, "Installed " + queue.length + " packs.");
        setTimeout(() => location.reload(), 900);
    } else {
        syncSelection();
    }
}

catalogInstall.onclick = installSelected;
catalogClear.onclick = () => {
    chosen.clear();
    catalogList.querySelectorAll(".catalog-select").forEach(box => { box.checked = false; });
    syncSelection();
};

async function fetchCatalog() {
    try {
        const response = await fetch("/api/packs/catalog", { cache: "no-store" });
        if (response.ok) return await response.json();
    } catch (error) { /* show the recovery copy below */ }
    return null;
}

/* Refresh through the device. Returns the catalogue to render plus the
 * device's own reason when the refresh failed, so the page can say why
 * (no uplink, or an uplink that was dropped for being out of range) instead of
 * quietly re-rendering the copy already on the device as "refreshed". */
/* The catalogue the phone can fetch by itself. Used when the device has no
 * usable uplink: the phone (joined to the PocketWiki access point, usually
 * with a connection of its own) can still show what is available, and the pack
 * itself can be installed from a file the phone already has. */
async function publicCatalog() {
    try {
        const response = await fetch("https://packs.educated.space/index.json", { cache: "no-store" });
        if (response.ok) return await response.json();
    } catch (error) { /* fall back to the copy stored on the device */ }
    return null;
}

async function syncCatalog() {
    /* The device fetches the catalogue over its own uplink, which can be slow
     * or absent; stop waiting instead of spinning forever. */
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 60000);
    try {
        const response = await fetch("/api/packs/catalog/sync",
                                     { method: "POST", signal: controller.signal });
        if (response.ok) return { catalog: await response.json(), error: null };
        const reason = (await response.text()).trim();
        return { catalog: await fetchCatalog(), error: reason || "PocketWiki could not refresh the catalogue." };
    } catch (error) {
        if (error.name === "AbortError") {
            return { catalog: await fetchCatalog(),
                     error: "PocketWiki took too long to refresh the catalogue. Check the Wi-Fi uplink and try again." };
        }
        /* The device did not answer at all; a phone with its own internet
         * connection can still use the public catalogue. */
        try {
            const response = await fetch("https://packs.educated.space/index.json", { cache: "no-store" });
            if (response.ok) return { catalog: await response.json(), error: null };
        } catch (fallbackError) { /* report the device failure below */ }
        return { catalog: await fetchCatalog(), error: "PocketWiki did not answer the catalogue refresh." };
    } finally {
        clearTimeout(timer);
    }
}

async function loadCatalog(manual) {
    if (manual) {
        catalogSync.disabled = true;
        showStatus(catalogStatus, "Refreshing catalogue…");
    }
    let catalog = null;
    let error = null;
    if (manual) {
        if (deviceUplink === false) {
            /* A device with no uplink cannot fetch the catalogue, so do not
             * wait on it: use the phone's own connection instead. */
            catalog = await publicCatalog() || await fetchCatalog();
            error = catalog
                ? "This device has no Wi-Fi uplink, so it cannot download packs. Showing the online catalogue: install a pack file from this phone instead."
                : "This device has no Wi-Fi uplink. Set Wi-Fi up under Manage, or install a pack file from this phone.";
        } else {
            const result = await syncCatalog();
            catalog = result.catalog;
            error = result.error;
        }
    } else {
        catalog = await fetchCatalog();
    }
    catalogSync.disabled = false;
    if (!catalog) {
        catalogList.innerHTML = '<p class="network-state">The catalogue is not available right now. Install a .pwp file or use a URL below.</p>';
        if (manual) showStatus(catalogStatus, error || "Could not refresh the catalogue. Connect PocketWiki to Wi-Fi with internet access and try again.");
        return;
    }
    renderCatalog(catalog);
    syncedAt.hidden = false;
    syncedAt.textContent = "Updated " + new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
    if (manual) {
        if (error) {
            showStatus(catalogStatus, error);
            return;
        }
        showStatus(catalogStatus, "Catalogue refreshed.");
        setTimeout(() => {
            if (catalogStatus.textContent === "Catalogue refreshed.") catalogStatus.textContent = "";
        }, 4000);
    }
}

catalogSync.onclick = () => loadCatalog(true);
loadCatalog(false);

document.querySelectorAll("[data-action]").forEach(button => {
    button.onclick = async () => {
        const row = button.closest("[data-pack-name]");
        const label = row ? row.querySelector("strong").textContent : "this pack";
        if (!window.confirm("Remove " + label + "? Its articles will disappear from the library.")) return;
        button.disabled = true;
        try {
            const response = await fetch("/api/packs/action?action=" + button.dataset.action +
                "&name=" + encodeURIComponent(button.dataset.name), { method: "POST" });
            const result = await response.json();
            if (result.ok) location.reload();
            else {
                button.disabled = false;
                showStatus(catalogStatus, result.error || "PocketWiki could not remove that pack.");
            }
        } catch (error) {
            button.disabled = false;
            showStatus(catalogStatus, "The connection was lost. The pack was not removed.");
        }
    };
});

const scanButton = document.getElementById("wifi-scan");
const networkList = document.getElementById("wifi-networks");
const ssidInput = document.getElementById("wifi-ssid");
const passwordInput = document.getElementById("wifi-password");
const wifiPanel = document.getElementById("wifi-panel");
const wifiToggle = document.getElementById("wifi-toggle");
const signal = rssi => rssi >= -60 ? "Strong" : rssi >= -75 ? "Fair" : "Weak";

async function scanWifi() {
    scanButton.disabled = true;
    scanButton.textContent = "Scanning…";
    networkList.replaceChildren();
    const state = document.createElement("p");
    state.className = "network-state";
    state.textContent = "Looking for nearby 2.4 GHz networks…";
    networkList.append(state);
    try {
        const response = await fetch("/api/wifi/scan");
        const result = await response.json();
        if (!response.ok) throw Error(result.error || "scan failed");
        const seen = new Set();
        const networks = result.networks.filter(network =>
            network.ssid && !seen.has(network.ssid) && seen.add(network.ssid));
        networkList.replaceChildren();
        if (!networks.length) {
            state.textContent = "No nearby networks found. Enter the network name below.";
            networkList.append(state);
        } else {
            networks.forEach(network => {
                const button = document.createElement("button");
                button.type = "button";
                button.className = "network-option";
                button.setAttribute("role", "option");
                button.setAttribute("aria-selected", "false");
                const name = document.createElement("span");
                name.className = "network-name";
                name.textContent = network.ssid;
                const detail = document.createElement("span");
                detail.className = "network-detail";
                detail.textContent = signal(network.rssi) + " · " + (network.open ? "Open" : "Secured");
                button.append(name, detail);
                button.onclick = () => {
                    networkList.querySelectorAll(".network-option").forEach(option => {
                        option.classList.remove("selected");
                        option.setAttribute("aria-selected", "false");
                    });
                    button.classList.add("selected");
                    button.setAttribute("aria-selected", "true");
                    ssidInput.value = network.ssid;
                    passwordInput.focus();
                };
                networkList.append(button);
            });
        }
    } catch (error) {
        state.textContent = "Could not scan nearby networks. Enter the name below or try again.";
        networkList.replaceChildren(state);
    } finally {
        scanButton.disabled = false;
        scanButton.textContent = "Scan nearby networks";
    }
}

function openWifiPanel() {
    wifiPanel.hidden = false;
    wifiToggle.textContent = "Hide Wi-Fi setup";
    document.getElementById("wifi-manager").scrollIntoView({ behavior: "smooth", block: "start" });
    scanWifi();
}

wifiToggle.onclick = () => {
    if (wifiPanel.hidden) openWifiPanel();
    else {
        wifiPanel.hidden = true;
        wifiToggle.textContent = "Set up Wi-Fi";
    }
};
scanButton.onclick = scanWifi;

document.getElementById("wifi-form").onsubmit = async event => {
    event.preventDefault();
    const status = document.getElementById("wifi-status");
    showStatus(status, "Saving Wi-Fi details…");
    try {
        const response = await fetch("/api/wifi/config", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ ssid: ssidInput.value, password: passwordInput.value })
        });
        const result = await response.json();
        if (!response.ok || !result.ok) throw Error(result.error || "PocketWiki could not save those details.");
        showStatus(status, "Saved. PocketWiki is connecting; this page will update shortly.");
        setTimeout(loadStatus, 1200);
    } catch (error) {
        showStatus(status, error.message || "PocketWiki could not save those details.");
    }
};

async function loadStatus() {
    const card = document.getElementById("wifi-status-card");
    let result = null;
    try {
        const response = await fetch("/api/wifi/status", { cache: "no-store" });
        if (response.ok) result = await response.json();
    } catch (error) { /* show the recovery state below */ }
    if (!result) {
        card.replaceChildren();
        const message = document.createElement("p");
        message.className = "network-state";
        message.textContent = "Could not read the Wi-Fi status. Try refreshing this page.";
        card.append(message);
        return;
    }
    deviceUplink = result.connected ? true : (result.uplink_suspended ? false : null);
    card.replaceChildren();
    const line = document.createElement("div");
    line.className = "wifi-status-line";
    const dot = document.createElement("span");
    dot.className = "status-dot" + (result.connected ? " on" : "");
    const label = document.createElement("strong");
    label.textContent = result.connected
        ? "Connected to " + result.ssid
        : (result.uplink_suspended
            ? "Wi-Fi dropped: " + result.ssid + " is out of range"
            : "PocketWiki access point");
    const ip = document.createElement("span");
    ip.className = "status-ip";
    ip.textContent = result.connected ? result.ip : result.ap_ip;
    line.append(dot, label, ip);
    card.append(line);

    const note = document.createElement("p");
    note.className = "muted";
    if (result.connected) {
        note.append("Pack downloads can use this connection. The access point stays available at ");
        const ap = document.createElement("strong");
        ap.textContent = result.ap_ssid + " (" + result.ap_ip + ")";
        note.append(ap);
        if (result.rssi) note.append(" · " + signal(result.rssi) + " signal");
    } else {
        note.append("Join ");
        const ap = document.createElement("strong");
        ap.textContent = result.ap_ssid + " (" + result.ap_ip + ")";
        note.append(ap, " to read and manage. Connect PocketWiki to another Wi-Fi network for catalogue downloads.");
        if (result.clients) note.append(" " + result.clients + " device" + (result.clients > 1 ? "s" : "") + " connected.");
    }
    const change = document.createElement("button");
    change.type = "button";
    change.className = "inline-action";
    change.textContent = "Change Wi-Fi";
    change.onclick = openWifiPanel;
    note.append(" ", change);
    card.append(note);
}

loadStatus();
