// Russian translation of the sysmon plugin. Keys are the English source strings.
#include "i18n/i18n.h"

namespace sysmon {

namespace {

const facet::i18n::Table& ru() {
    static const facet::i18n::Table table = {
        // Tile and gauges
        {"System monitor", "Мониторинг"},
        {"CPU {}", "ЦП {}"},
        {"RAM {}", "ОЗУ {}"},
        {"CPU", "ЦП"},
        {"RAM", "ОЗУ"},
        {"Temperature", "Температура"},
        {"Disk", "Диск"},

        // Processor
        {"Processor", "Процессор"},
        {"Load, last 5 min", "Нагрузка за 5 мин"},
        {"Model", "Модель"},
        {"Cores", "Ядра"},
        {"Frequency", "Частота"},
        {"{} GHz", "{} ГГц"},
        {"Load average", "Средняя нагрузка"},
        {"Processes", "Процессы"},
        {"{} running of {}", "{} активных из {}"},

        // Memory and disks
        {"Memory", "Память"},
        {"Swap", "Подкачка"},
        {"Disks", "Диски"},
        {"Read", "Чтение"},
        {"Write", "Запись"},

        // Network
        {"Network", "Сеть"},
        {"Download {}", "Приём {}"},
        {"Upload {}", "Отдача {}"},
        {"Scale: {}", "Шкала: {}"},

        // Sensors
        {"Temperatures", "Температуры"},
        {"NVMe SSD", "NVMe SSD"},
        {"Mainboard (ACPI)", "Плата (ACPI)"},
        {"Chipset", "Чипсет"},
        {"Wi-Fi", "Wi-Fi"},
        {"Graphics", "Видеокарта"},

        // Processes and system
        {"Top processes", "Самые активные процессы"},
        {"Measuring…", "Измеряю…"},
        {"System", "Система"},
        {"Uptime", "Работает"},
        {"Battery", "Батарея"},
        {"charging", "заряжается"},
        {"on battery", "от батареи"},
        {"full", "заряжена"},
        {"plugged in", "от сети"},
        {"not charging", "не заряжается"},
        {"status unknown", "состояние неизвестно"},

        // Units
        {"{} B", "{} Б"},
        {"{} KB", "{} КБ"},
        {"{} MB", "{} МБ"},
        {"{} GB", "{} ГБ"},
        {"{} TB", "{} ТБ"},
        {"{}/s", "{}/с"},
        {"{} d {} h", "{} д {} ч"},
        {"{} h {} min", "{} ч {} мин"},
        {"{} min", "{} мин"},
    };
    return table;
}

}  // namespace

void register_translations(facet::i18n::Catalog& catalog) { catalog.add("ru", ru()); }

}  // namespace sysmon
