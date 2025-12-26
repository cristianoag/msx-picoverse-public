
typedef struct  {
    uint8_t id;
    char* name;
} Manufacturer;

Manufacturer const manufacturers[21] = {
    { 1,   "Panasonic" },
    { 2,   "Toshiba" },
    { 3,   "SanDisk" },
    { 4,   "SMI-S" },
    { 6,   "Renesas" },
    { 17,  "Dane-Elec" },
    { 19,  "KingMax" },
    { 21,  "Samsung" },
    { 24,  "Infineon" },
    { 26,  "PQI" },
    { 27,  "Sony" },
    { 28,  "Transcend" },
    { 29,  "A-DATA" },
    { 31,  "SiliconPower" },
    { 39,  "Verbatim" },
    { 62,  "RetroWiki VHD MiSTer" },
    { 65,  "OKI" },
    { 115, "SilverHT" },
    { 116, "RetroWiki VHD MiST" },
    { 137, "L.Data" },
    { 0,   "Generic" }
};

const char* getManufacturerName(uint8_t id) {
    
    for (uint8_t i=0;i<20;i++)
    {
            if ((uint8_t)manufacturers[i].id==id)
                return manufacturers[i].name;
    }

    return "Generic";
}  