import * as exposes from "zigbee-herdsman-converters/lib/exposes";
import * as m from "zigbee-herdsman-converters/lib/modernExtend";

const e = exposes.presets;
const ea = exposes.access;

// Custom converters for the per-endpoint controls. We deliberately do NOT set
// an `endpoint()` map: Z2M would then publish per-endpoint subtopics
// (zigbee2mqtt/<name>/led, .../display) on every device message — chatty when
// sensors report every 10s but the control state hasn't changed. With endpoints
// referenced explicitly here, properties land only in the main topic.
const fz_local = {
    co2_led: {
        cluster: "genOnOff",
        type: ["attributeReport", "readResponse"],
        convert: (model, msg) => {
            if (msg.endpoint.ID !== 1 || msg.data.onOff === undefined) return;
            return {co2_led: msg.data.onOff ? "ON" : "OFF"};
        },
    },
    display_state: {
        cluster: "genOnOff",
        type: ["attributeReport", "readResponse"],
        convert: (model, msg) => {
            if (msg.endpoint.ID !== 2 || msg.data.onOff === undefined) return;
            return {display: msg.data.onOff ? "ON" : "OFF"};
        },
    },
    display_brightness: {
        cluster: "genLevelCtrl",
        type: ["attributeReport", "readResponse"],
        convert: (model, msg) => {
            if (msg.endpoint.ID !== 2 || msg.data.currentLevel === undefined) return;
            return {display_brightness: msg.data.currentLevel};
        },
    },
};

const tz_local = {
    co2_led: {
        key: ["co2_led"],
        convertSet: async (entity, key, value, meta) => {
            const ep = meta.device.getEndpoint(1);
            const v = value === "ON" || value === true;
            await ep.command("genOnOff", v ? "on" : "off", {}, {});
            return {state: {co2_led: v ? "ON" : "OFF"}};
        },
        convertGet: async (entity, key, meta) => {
            await meta.device.getEndpoint(1).read("genOnOff", ["onOff"]);
        },
    },
    display: {
        key: ["display"],
        convertSet: async (entity, key, value, meta) => {
            const ep = meta.device.getEndpoint(2);
            const v = value === "ON" || value === true;
            await ep.command("genOnOff", v ? "on" : "off", {}, {});
            return {state: {display: v ? "ON" : "OFF"}};
        },
        convertGet: async (entity, key, meta) => {
            await meta.device.getEndpoint(2).read("genOnOff", ["onOff"]);
        },
    },
    display_brightness: {
        key: ["display_brightness"],
        convertSet: async (entity, key, value, meta) => {
            const ep = meta.device.getEndpoint(2);
            const level = Math.max(0, Math.min(254, Number.parseInt(value, 10)));
            await ep.command("genLevelCtrl", "moveToLevel", {level, transtime: 0}, {});
            return {state: {display_brightness: level}};
        },
        convertGet: async (entity, key, meta) => {
            await meta.device.getEndpoint(2).read("genLevelCtrl", ["currentLevel"]);
        },
    },
};

export default {
    zigbeeModel: ["SCD41-XIAO"],
    model: "SCD41-XIAO",
    vendor: "Sensirion",
    description: "SCD41 CO2, temperature and humidity sensor",
    // Standard sensor clusters — modernExtend handles fz/tz/exposes/configure
    // (bind + configureReporting) for us.
    extend: [m.temperature(), m.humidity(), m.co2()],
    // Custom controls (LED on ep 1, optional OLED display on ep 2).
    fromZigbee: [fz_local.co2_led, fz_local.display_state, fz_local.display_brightness],
    toZigbee: [tz_local.co2_led, tz_local.display, tz_local.display_brightness],
    exposes: (device) => {
        const list = [e.binary("co2_led", ea.ALL, "ON", "OFF").withDescription("CO2 level indicator LED")];
        if (device?.getEndpoint?.(2)) {
            list.push(
                e.binary("display", ea.ALL, "ON", "OFF").withDescription("OLED display power"),
                e.numeric("display_brightness", ea.ALL).withValueMin(0).withValueMax(254).withDescription("OLED display brightness (0-254)"),
            );
        }
        return list;
    },
    configure: async (device, coordinatorEndpoint) => {
        const ep1 = device.getEndpoint(1);
        await ep1.bind("genOnOff", coordinatorEndpoint);

        const ep2 = device.getEndpoint(2);
        if (ep2) {
            await ep2.bind("genOnOff", coordinatorEndpoint);
            await ep2.bind("genLevelCtrl", coordinatorEndpoint);
        }
    },
};
