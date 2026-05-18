import {temperature, humidity, on_off} from "zigbee-herdsman-converters/converters/fromZigbee";
import {on_off as tz_on_off} from "zigbee-herdsman-converters/converters/toZigbee";

const fzLocal = {
    co2: {
        cluster: "msCO2",
        type: ["attributeReport", "readResponse"],
        convert: (model, msg, publish, options, meta) => {
            if (msg.data.measuredValue !== undefined) {
                return {co2: Math.round(msg.data.measuredValue * 1000000)};
            }
        },
    },
};

export default {
    zigbeeModel: ["SCD41-XIAO"],
    model: "SCD41-XIAO",
    vendor: "Sensirion",
    description: "SCD41 CO2, temperature and humidity sensor",
    fromZigbee: [temperature, humidity, fzLocal.co2, on_off],
    toZigbee: [tz_on_off],
    exposes: [
        {type: "numeric", name: "co2", label: "CO2", property: "co2", access: 1, unit: "ppm", description: "Carbon dioxide concentration"},
        {type: "numeric", name: "temperature", label: "Temperature", property: "temperature", access: 1, unit: "°C", description: "Measured temperature"},
        {type: "numeric", name: "humidity", label: "Humidity", property: "humidity", access: 1, unit: "%", description: "Measured relative humidity"},
        {type: "binary", name: "state", label: "CO2 LED", property: "state", access: 7, value_on: "ON", value_off: "OFF", description: "CO2 level indicator LED"},
    ],
    configure: async (device, coordinatorEndpoint, definition) => {
        const endpoint = device.getEndpoint(1);

        // Bind clusters so we can configure reporting
        await endpoint.bind("msTemperatureMeasurement", coordinatorEndpoint);
        await endpoint.bind("msRelativeHumidity", coordinatorEndpoint);
        await endpoint.bind("msCO2", coordinatorEndpoint);
        await endpoint.bind("genOnOff", coordinatorEndpoint);

        // Configure reporting: min 10s, max 60s
        await endpoint.configureReporting("msTemperatureMeasurement", [
            {attribute: "measuredValue", minimumReportInterval: 10, maximumReportInterval: 60, reportableChange: 10},
        ]);
        await endpoint.configureReporting("msRelativeHumidity", [
            {attribute: "measuredValue", minimumReportInterval: 10, maximumReportInterval: 60, reportableChange: 100},
        ]);
        await endpoint.configureReporting("msCO2", [
            {attribute: "measuredValue", minimumReportInterval: 10, maximumReportInterval: 60, reportableChange: 0},
        ]);
    },
};
