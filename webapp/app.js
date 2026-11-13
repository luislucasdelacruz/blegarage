const SERVICE_UUID = "c9af0000-1b19-4f33-890b-0d6a1f98b6a1";
const STATUS_UUID = "c9af0001-1b19-4f33-890b-0d6a1f98b6a1";
const CONTROL_UUID = "c9af0002-1b19-4f33-890b-0d6a1f98b6a1";
const CHANGE_PASSKEY_UUID = "c9af0003-1b19-4f33-890b-0d6a1f98b6a1";
const CHANGE_OTA_PASSWORD_UUID = "c9af0004-1b19-4f33-890b-0d6a1f98b6a1";

const connectBtn = document.getElementById("connect-btn");
const toggleBtn = document.getElementById("toggle-btn");
const statusText = document.getElementById("status-text");

const currentPasskeyInput = document.getElementById("current-passkey");
const newPasskeyInput = document.getElementById("new-passkey");
const changePasskeyBtn = document.getElementById("change-passkey-btn");

const currentOtaPasswordInput = document.getElementById("current-ota-password");
const newOtaPasswordInput = document.getElementById("new-ota-password");
const changeOtaPasswordBtn = document.getElementById("change-ota-password-btn");

const settingsToggleBtn = document.getElementById("settings-toggle-btn");
const settingsSection = document.getElementById("settings-section");
const settingsError = document.getElementById("settings-error");

let statusCharacteristic;
let controlCharacteristic;
let changePasskeyCharacteristic;
let changeOtaPasswordCharacteristic;

function buildChangeSecretPayload(currentValue, newValue) {
  const buffer = new ArrayBuffer(8);
  const view = new DataView(buffer);
  view.setUint32(0, currentValue, true);
  view.setUint32(4, newValue, true);
  return new Uint8Array(buffer);
}

function updateStatus(value) {
  const isOpen = value === 1;
  statusText.textContent = isOpen ? "Open" : "Closed";
  statusText.classList.remove(
    "status--unknown",
    "status--open",
    "status--closed",
  );
  statusText.classList.add(isOpen ? "status--open" : "status--closed");
}

connectBtn.addEventListener("click", async () => {
  try {
    statusText.textContent = "Connecting...";

    const device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SERVICE_UUID] }],
    });

    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE_UUID);

    statusCharacteristic = await service.getCharacteristic(STATUS_UUID);
    controlCharacteristic = await service.getCharacteristic(CONTROL_UUID);
    changePasskeyCharacteristic =
      await service.getCharacteristic(CHANGE_PASSKEY_UUID);
    changeOtaPasswordCharacteristic = await service.getCharacteristic(
      CHANGE_OTA_PASSWORD_UUID,
    );

    const initialValue = await statusCharacteristic.readValue();
    updateStatus(initialValue.getUint8(0));

    await statusCharacteristic.startNotifications();
    statusCharacteristic.addEventListener(
      "characteristicvaluechanged",
      (event) => {
        updateStatus(event.target.value.getUint8(0));
      },
    );

    toggleBtn.disabled = false;
    changePasskeyBtn.disabled = false;
    changeOtaPasswordBtn.disabled = false;
  } catch (error) {
    console.error(error);
    statusText.textContent = `Connection failed: ${error.message}`;
  }
});

toggleBtn.addEventListener("click", async () => {
  try {
    await controlCharacteristic.writeValue(new Uint8Array([0]));
  } catch (error) {
    console.error(error);
  }
});

settingsToggleBtn.addEventListener("click", () => {
  settingsSection.hidden = !settingsSection.hidden;
  settingsError.textContent = "";
});

changePasskeyBtn.addEventListener("click", async () => {
  try {
    const payload = buildChangeSecretPayload(
      Number(currentPasskeyInput.value),
      Number(newPasskeyInput.value),
    );
    await changePasskeyCharacteristic.writeValue(payload);
    currentPasskeyInput.value = "";
    newPasskeyInput.value = "";
    settingsError.textContent = "";
  } catch (error) {
    console.error(error);
    settingsError.textContent =
      "Failed to update passkey - check the current value.";
  }
});

changeOtaPasswordBtn.addEventListener("click", async () => {
  try {
    const payload = buildChangeSecretPayload(
      Number(currentOtaPasswordInput.value),
      Number(newOtaPasswordInput.value),
    );
    await changeOtaPasswordCharacteristic.writeValue(payload);
    currentOtaPasswordInput.value = "";
    newOtaPasswordInput.value = "";
    settingsError.textContent = "";
  } catch (error) {
    console.error(error);
    settingsError.textContent =
      "Failed to update OTA password - check the current value.";
  }
});
