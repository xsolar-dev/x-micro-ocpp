// matth-x/ArduinoOcpp
// Copyright Matthias Akstaller 2019 - 2022
// MIT License

#include <ArduinoOcpp/Core/FilesystemAdapter.h>
#include <ArduinoOcpp/Core/ConfigurationOptions.h> //FilesystemOpt
#include <ArduinoOcpp/Debug.h>

//#ifndef AO_DEACTIVATE_FLASH
#if 1

//Set default parameters; assume usage with Arduino if no build flags are present
#ifndef AO_USE_FILEAPI
#if defined(ESP32)
#define AO_USE_FILEAPI ARDUINO_LITTLEFS
#else
#define AO_USE_FILEAPI ARDUINO_SPIFFS
#endif
#endif //ndef AO_USE_FILEAPI

/*
 * Platform specific implementations. Currently supported:
 *     - Arduino LittleFs
 *     - Arduino SPIFFS
 *     - ESP-IDF SPIFFS
 * 
 * You can add support for any file system by passing custom adapters to the initialize
 * function of ArduinoOcpp
 */

#if AO_USE_FILEAPI == ARDUINO_LITTLEFS
#include <LITTLEFS.h>
#define USE_FS LITTLEFS
#elif AO_USE_FILEAPI == ARDUINO_SPIFFS
#include <FS.h>
#define USE_FS SPIFFS
#elif AO_USE_FILEAPI == ESPIDF_SPIFFS
#include <sys/stat.h>
#include "esp_spiffs.h"
#endif


#if AO_USE_FILEAPI == ARDUINO_LITTLEFS || AO_USE_FILEAPI == ARDUINO_SPIFFS

namespace ArduinoOcpp {
namespace EspWiFi {

class ArduinoFileAdapter : public FileAdapter {
    File file;
public:
    ArduinoFileAdapter(File&& file) : file(file) {}

    ~ArduinoFileAdapter() {
        if (file) {
            file.close();
        }
    }
    
    int read() override;
    size_t read(char *buf, size_t len) override;
    size_t write(const char *buf, size_t len) override;
    size_t seek(size_t offset) override;
};

class ArduinoFilesystemAdapter : public FilesystemAdapter {
private:
    bool valid = false;
    FilesystemOpt config;
public:
    ArduinoFilesystemAdapter(FilesystemOpt config) : config(config) {
        valid = true;

        if (config.mustMount()) { 
#if AO_USE_FILEAPI == ARDUINO_LITTLEFS
            if(!USE_FS.begin(config.formatOnFail())) {
                AO_DBG_ERR("Error while mounting LITTLEFS");
                valid = false;
            }
#elif AO_USE_FILEAPI == ARDUINO_SPIFFS
            //ESP8266
            SPIFFSConfig cfg;
            cfg.setAutoFormat(config.formatOnFail());
            SPIFFS.setConfig(cfg);

            if (!SPIFFS.begin()) {
                AO_DBG_ERR("Unable to initialize: unable to mount SPIFFS");
                valid = false;
            }
#else
#error
#endif
        } //end if mustMount()
    }

    ~ArduinoFilesystemAdapter() {
        if (config.mustMount()) {
            USE_FS.end();
        }
    }

    operator bool() {return valid;}

    int stat(const char *path, size_t *size) override {
        if (!USE_FS.exists(path)) {
            return -1;
        }
        File f = USE_FS.open(path, "r");
        if (!f) {
            return -1;
        }

        int status = -1;
        if (f.isFile()) {
            size = f.size();
            status = 0;
        } else {
            //fetch more information for directory when ArduinoOcpp also uses them
            //status = 0;
        }

        f.close();
        return status;
    }

    std::unique_ptr<FileAdapter> open(const char *fn, const char *mode) override {
        File file = USE_FS.open(fn, mode);
        if (file && file.isFile()) {
            return std::unique_ptr<FileAdapter>(new ArduinoFileAdapter(std::move(file)));
        } else {
            return nullptr;
        }
    }
    bool remove(const char *fn) override {
        return USE_FS.remove(fn);
    };
};

std::unique_ptr<FilesystemAdapter> makeDefaultFilesystemAdapter(FilesystemOpt config) {

    if (!config.accessAllowed()) {
            AO_DBG_DEBUG("Access to Arduino FS not allowed by config");
            return nullptr;
    }

    auto fs = std::unique_ptr<FilesystemAdapter>(
        new ArduinoFilesystemAdapter(config)
    );

    if (*fs) {
        return fs;
    } else {
        return nullptr;
    }
}

} //end namespace EspWiFi
} //end namespace ArduinoOcpp

#elif AO_USE_FILEAPI == ESPIDF_SPIFFS

namespace ArduinoOcpp {
namespace EspWiFi {

class EspIdfFileAdapter : public FileAdapter {
    FILE *file {nullptr};
public:
    EspIdfFileAdapter(FILE *file) : file(file) {}

    ~EspIdfFileAdapter() {
        fclose(file);
    }

    size_t read(char *buf, size_t len) override {
        return fread(buf, 1, len, file);
    }

    size_t write(const char *buf, size_t len) override {
        return fwrite(buf, 1, len, file);
    }

    size_t seek(size_t offset) override {
        return fseek(file, offset, SEEK_SET);
    }

    int read() {
        return fgetc(file);
    }
};

class EspIdfFilesystemAdapter : public FilesystemAdapter {
public:
    FilesystemOpt config;
public:
    EspIdfFilesystemAdapter(FilesystemOpt config) : config(config) { }

    ~EspIdfFilesystemAdapter() {
        if (config.mustMount()) {
            esp_vfs_spiffs_unregister("ao"); //partition label
            AO_DBG_DEBUG("SPIFFS unmounted");
        }
    }

    int stat(const char *path, size_t *size) override {
        struct ::stat st;
        auto ret = ::stat(path, &st);
        if (ret == 0) {
            *size = st.st_size;
        }
        return ret;
    }

    std::unique_ptr<FileAdapter> open(const char *fn, const char *mode) override {
        auto file = fopen(fn, mode);
        if (file) {
            return std::unique_ptr<FileAdapter>(new EspIdfFileAdapter(std::move(file)));
        } else {
            AO_DBG_DEBUG("Failed to open file path %s", fn);
            return nullptr;
        }
    }

    bool remove(const char *fn) override {
        return unlink(fn) == 0;
    }
};

std::unique_ptr<FilesystemAdapter> makeDefaultFilesystemAdapter(FilesystemOpt config) {

    if (!config.accessAllowed()) {
            AO_DBG_DEBUG("Access to ESP-IDF SPIFFS not allowed by config");
            return nullptr;
    }

    bool mounted = true;

    if (config.mustMount()) {
        mounted = false;
        
        esp_vfs_spiffs_conf_t conf = {
            .base_path = AO_FILENAME_PREFIX,
            .partition_label = "ao", //also see deconstructor
            .max_files = 5,
            .format_if_mount_failed = config.formatOnFail()
        };

        esp_err_t ret = esp_vfs_spiffs_register(&conf);

        if (ret == ESP_OK) {
            mounted = true;
            AO_DBG_DEBUG("SPIFFS mounted");
        } else {
            if (ret == ESP_FAIL) {
                AO_DBG_ERR("Failed to mount or format filesystem");
            } else if (ret == ESP_ERR_NOT_FOUND) {
                AO_DBG_ERR("Failed to find SPIFFS partition");
            } else {
                AO_DBG_ERR("Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
            }
        }
    }

    if (mounted) {
        return std::unique_ptr<FilesystemAdapter>(new EspIdfFilesystemAdapter(config));
    } else {
        return nullptr;
    }
}

} //end namespace EspWiFi
} //end namespace ArduinoOcpp

#endif

#endif