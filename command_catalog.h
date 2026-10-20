#pragma once
#include <QString>
#include <QStringList>
#include <array>

// Назначения взяты из исходников БЦВМ; установленная прошивка и проводка проверяются отдельно.
// Источник: cyclo_actions.c БЦВМ, ревизия c8e14dcfe6e13d703044beca58c3dc2ee2a8c32b.
struct CommandDefinition {
    const char *key;
    const char *peer;
    unsigned mask;
};
inline constexpr const char *kCommandCatalogVersion = "bcvm-c8e14dc-1";
inline constexpr std::array<CommandDefinition, 33> kCommandDefinitions{{
    {"START_HELIUM_PURGE_A", "", 0},
    {"START_HELIUM_PURGE_B", "", 0},
    {"IGNITE_PYRO_CANDLES_ENGINES_9_TO_12", "bkhoa", 3},
    {"IGNITE_PYRO_CANDLES_ENGINES_1_TO_8", "", 0},
    {"FIRE_STARTING_CHAMBER_ENGINES_9_TO_12", "bkhoa", 28},
    {"FIRE_STARTING_CHAMBER_ENGINES_1_TO_8", "", 0},
    {"LIFT_OFF_CONTACT", "bkhoa", 128},
    {"SAFETY_SYSTEM_ACTIVATION", "", 0},
    {"EXPAND_SAZ_PARAMETERS", "", 0},
    {"ROLL_PROGRAM_BEGIN", "", 0},
    {"ROLL_PROGRAM_END", "", 0},
    {"PITCH_PROGRAM_BEGIN", "", 0},
    {"PITCH_PROGRAM_END", "", 0},
    {"THRUST_DEREGULATION", "", 0},
    {"DISABLE_SAZ", "", 0},
    {"CLOSE_MAIN_VALVES_ENGINES_9_TO_12", "bkhoa", 96},
    {"CLOSE_MAIN_VALVES_ENGINES_8_TO_12", "", 0},
    {"HELIUM_PURGE_NK3VK_ENGINE", "", 0},
    {"SEPARATE_BLOCKS_A_AND_B", "", 0},
    {"IGNITE_PYRO_CANDLE", "bkpob", 3},
    {"OPEN_OXIDIZER_AND_FUEL_VALVES", "bkpob", 28},
    {"FAIRING_SEPARATION", "", 0},
    {"TRANSITION_TO_FINAL_THRUST_STAGE", "", 0},
    {"SHUTDOWN_ENGINE_BLOCK_B", "bkpob", 96},
    {"ENABLE_POSTFIRE_PURGE", "", 0},
    {"DISABLE_POSTFIRE_PURGE", "", 0},
    {"SOOS_ACTIVATION", "", 0},
    {"COMBUSTION_CHAMBER_PURGE", "", 0},
    {"SECOND_IGNITE_PYRO_CANDLE", "", 0},
    {"SECOND_OPEN_OXIDIZER_AND_FUEL_VALVES", "", 0},
    {"DEACTIVATE_SOOS", "", 0},
    {"TRANSITION_TO_FINAL_THRUST_STAGE_AGAIN", "", 0},
    {"SHUTDOWN_ENGINE_BLOCK_B_AGAIN", "", 0},
}};
inline const CommandDefinition *findCommand(const QString &key) {
    for (const auto &definition : kCommandDefinitions)
        if (key == QLatin1String(definition.key)) return &definition;
    return nullptr;
}
inline QStringList expectedSignals(const CommandDefinition &definition) {
    QStringList result;
    for (int bit = 0; bit < 8; ++bit)
        if (definition.mask & (1u << bit))
            result.append(QStringLiteral("%1.yps_bkd.0.bit.%2")
                               .arg(QLatin1String(definition.peer)).arg(bit));
    return result;
}
