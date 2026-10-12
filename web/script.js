const releaseEndpoint = "https://api.github.com/repos/shifatsrm09/FLUX/releases/latest";
const setupLink = document.querySelector("#setup-download");
const portableLink = document.querySelector("#portable-download");
const note = document.querySelector("#release-note");
const status = document.querySelector("#release-status");

async function loadLatestRelease() {
  try {
    const response = await fetch(releaseEndpoint, {
      headers: { Accept: "application/vnd.github+json" },
      cache: "no-store",
    });
    if (!response.ok) throw new Error(`GitHub returned ${response.status}`);

    const release = await response.json();
    const assets = release.assets ?? [];
    const setup = assets.find((asset) => /-Setup\.exe$/i.test(asset.name));
    const portable = assets.find((asset) => /-win64-portable\.zip$/i.test(asset.name));

    if (setup) setupLink.href = setup.browser_download_url;
    if (portable) portableLink.href = portable.browser_download_url;
    if (!setup || !portable) throw new Error("One or more download assets were not found");

    const version = release.tag_name || release.name || "Latest";
    note.textContent = `${version} · ${formatSize(setup.size)} installer · ${formatSize(portable.size)} portable`;
    status.textContent = "THE LATEST RELEASE";
  } catch (error) {
    // The releases page remains a working fallback if the API is unavailable.
    note.textContent = "Latest downloads on GitHub · Windows 10/11";
    status.textContent = "READY TO DOWNLOAD";
  }
}

function formatSize(bytes) {
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

loadLatestRelease();

const networkCheckDialog = document.querySelector("#network-check-dialog");
const networkCheckResult = document.querySelector("#network-check-result");
const networkCheckIndicator = document.querySelector("#network-check-indicator");
const networkCheckAgain = document.querySelector("#network-check-again");
const networkCheckUrl = "http://172.16.50.4/";
let activeNetworkCheck = null;
let networkCheckRun = 0;
let localNetworkPermission = "unknown";
getLocalNetworkPermission().then((permission) => {
  localNetworkPermission = permission;
});

document.querySelector("#network-check-open").addEventListener("click", runNetworkCheck);
networkCheckAgain.addEventListener("click", runNetworkCheck);

document.querySelector("#network-check-close").addEventListener("click", () => {
  networkCheckDialog.close();
});

networkCheckDialog.addEventListener("close", () => {
  networkCheckRun += 1;
  activeNetworkCheck?.abort();
  activeNetworkCheck = null;
});

async function runNetworkCheck() {
  if (!networkCheckDialog.open) networkCheckDialog.showModal();

  activeNetworkCheck?.abort();
  const run = ++networkCheckRun;

  networkCheckResult.textContent = "Checking 172.16.50.4…";
  networkCheckIndicator.dataset.state = "checking";
  networkCheckAgain.disabled = true;

  const controller = new AbortController();
  activeNetworkCheck = controller;
  const timeout = window.setTimeout(() => controller.abort(), 15000);

  try {
    if (localNetworkPermission === "denied") {
      networkCheckResult.textContent = "Local network access is blocked for this site. Allow it in your browser’s site settings, then try again.";
      networkCheckIndicator.dataset.state = "failure";
      return;
    }

    await fetch(networkCheckUrl, {
      method: "GET",
      mode: "no-cors",
      cache: "no-store",
      redirect: "follow",
      signal: controller.signal,
      targetAddressSpace: "local",
    });

    if (run === networkCheckRun) {
      networkCheckResult.textContent = "FLUX should work on this network.";
      networkCheckIndicator.dataset.state = "success";
    }
  } catch (error) {
    if (run === networkCheckRun) {
      const permission = localNetworkPermission === "unknown"
        ? await getLocalNetworkPermission()
        : localNetworkPermission;
      localNetworkPermission = permission;
      if (permission === "denied") {
        networkCheckResult.textContent = "Local network access is blocked for this site. Allow it in your browser’s site settings, then try again.";
      } else if (permission === "unsupported") {
        networkCheckResult.textContent = "This browser can’t confirm local network access. Try the latest Chrome or Edge, then allow access if prompted.";
      } else if (permission === "prompt") {
        networkCheckResult.textContent = "Allow local network access for this site in your browser’s site settings, then reload and try again.";
      } else if (controller.signal.aborted) {
        networkCheckResult.textContent = "No response from the FLUX network source. Check that you’re on the right network and try again.";
      } else if (permission === "granted") {
        networkCheckResult.textContent = "Your browser allows local network access, but 172.16.50.4 didn’t respond. Check your network and try again.";
      } else {
        networkCheckResult.textContent = "The browser blocked this local network check. Try the latest Chrome or Edge and allow access for this site.";
      }
      networkCheckIndicator.dataset.state = "failure";
    }
  } finally {
    window.clearTimeout(timeout);
    if (run === networkCheckRun) {
      activeNetworkCheck = null;
      networkCheckAgain.disabled = false;
    }
  }
}

async function getLocalNetworkPermission() {
  if (!navigator.permissions?.query) return "unsupported";
  try {
    const permission = await navigator.permissions.query({ name: "local-network" });
    return permission.state;
  } catch {
    return "unsupported";
  }
}
