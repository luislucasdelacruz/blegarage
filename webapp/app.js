const SERVICE_UUID = "c9af0000-1b19-4f33-890b-0d6a1f98b6a1";
const STATUS_UUID = "c9af0001-1b19-4f33-890b-0d6a1f98b6a1";
const CONTROL_UUID = "c9af0002-1b19-4f33-890b-0d6a1f98b6a1";

const connectBtn = document.getElementById("connect-btn");
const toggleBtn = document.getElementById("toggle-btn");
const statusText = document.getElementById("status-text");

let statusCharacteristic;
let controlCharacteristic;

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
  } catch (error) {
    console.error(error);
    statusText.textContent = "Connection failed";
  }
});

toggleBtn.addEventListener("click", async () => {
  try {
    await controlCharacteristic.writeValue(new Uint8Array([0]));
  } catch (error) {
    console.error(error);
  }
});
