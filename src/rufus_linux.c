/*
 * Rufus: The Reliable USB Formatting Utility (Linux Port)
 * Copyright © 2026 Pete Batard <pete@akeo.ie> & DeepMind AI Team
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <fcntl.h>
#include <dirent.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#include <openssl/evp.h>

#define APP_TITLE "Rufus (Arch Linux Port)"
#define APP_VERSION "1.0.0"

// Global UI widgets
GtkWidget *window;
GtkWidget *combo_device;
GtkWidget *check_show_all_drives;
GtkWidget *combo_boot_selection;
GtkWidget *btn_select_iso;
GtkWidget *lbl_iso_name;
GtkWidget *combo_partition_scheme;
GtkWidget *combo_target_system;
GtkWidget *entry_volume_label;
GtkWidget *combo_filesystem;
GtkWidget *combo_cluster_size;
GtkWidget *progress_bar;
GtkWidget *lbl_status;
GtkWidget *lbl_speed_eta;
GtkWidget *text_log;
GtkTextBuffer *log_buffer;
GtkWidget *btn_start;
GtkWidget *btn_close;
GtkWidget *btn_log_toggle;
GtkWidget *scrolled_log;

// Global settings/state
char selected_iso_path[1024] = {0};
unsigned long long selected_iso_size = 0;
char system_root_disk[32] = {0};

typedef struct {
    char name[32];     // e.g. "sdb"
    char model[128];   // e.g. "Cruzer Blade"
    char size[32];     // e.g. "14.9G"
    int removable;     // 1 for USB, 0 for internal
    int read_only;     // 1 for read-only, 0 for read-write
} DiskInfo;

#define MAX_DISKS 64
DiskInfo disks[MAX_DISKS];
int disk_count = 0;

// Log function
void log_to_ui(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    // Keep console output too
    printf("%s", buf);

    // Append to textview buffer (must be run on main thread)
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(log_buffer, &end);
    gtk_text_buffer_insert(log_buffer, &end, buf, -1);
    
    // Auto-scroll to bottom
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled_log));
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
}

// Thread-safe logging structures
typedef struct {
    char *message;
} LogMsgData;

static gboolean idle_log_msg(gpointer data) {
    LogMsgData *msg_data = (LogMsgData *)data;
    log_to_ui("%s", msg_data->message);
    g_free(msg_data->message);
    g_free(msg_data);
    return FALSE;
}

void log_from_thread(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *buf = g_strdup_vprintf(format, args);
    va_end(args);

    LogMsgData *data = g_new0(LogMsgData, 1);
    data->message = buf;
    g_idle_add(idle_log_msg, data);
}

// Thread-safe status structures
typedef struct {
    double progress;
    char *status;
    char *speed_eta;
} UIProgressData;

static gboolean idle_update_progress(gpointer data) {
    UIProgressData *progress_data = (UIProgressData *)data;
    if (progress_data->progress >= 0.0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), progress_data->progress);
    }
    if (progress_data->status) {
        gtk_label_set_text(GTK_LABEL(lbl_status), progress_data->status);
        g_free(progress_data->status);
    }
    if (progress_data->speed_eta) {
        gtk_label_set_text(GTK_LABEL(lbl_speed_eta), progress_data->speed_eta);
        g_free(progress_data->speed_eta);
    }
    g_free(progress_data);
    return FALSE;
}

void update_ui_progress(double progress, const char *status, const char *speed_eta) {
    UIProgressData *data = g_new0(UIProgressData, 1);
    data->progress = progress;
    data->status = status ? g_strdup(status) : NULL;
    data->speed_eta = speed_eta ? g_strdup(speed_eta) : NULL;
    g_idle_add(idle_update_progress, data);
}

// Detect the disk where / is mounted
void detect_system_root_disk(void) {
    FILE *fp = popen("findmnt -n -o SOURCE / 2>/dev/null", "r");
    if (fp) {
        char buf[256];
        if (fgets(buf, sizeof(buf), fp)) {
            // Check for /dev/nvme0n1p2 or /dev/sda1
            if (strncmp(buf, "/dev/", 5) == 0) {
                // Find where partition starts
                char *p = buf + 5;
                int len = 0;
                while (p[len] && p[len] != '\n' && p[len] != ' ' && p[len] != '[') {
                    len++;
                }
                p[len] = '\0';
                
                // If it is nvme0n1p2, root disk is nvme0n1. If sda2, root disk is sda.
                if (strncmp(p, "nvme", 4) == 0) {
                    // For nvme0n1p2, copy nvme0n1
                    char *part = strstr(p, "p");
                    if (part) {
                        *part = '\0';
                    }
                } else {
                    // For sda2, remove digits at end
                    int i = strlen(p) - 1;
                    while (i >= 0 && p[i] >= '0' && p[i] <= '9') {
                        p[i] = '\0';
                        i--;
                    }
                }
                snprintf(system_root_disk, sizeof(system_root_disk), "%s", p);
            }
        }
        pclose(fp);
    }
    printf("Detected system root disk to protect: /dev/%s\n", system_root_disk);
}

// Parse lsblk command to find USB disks
void refresh_devices(void) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo_device));
    disk_count = 0;

    detect_system_root_disk();

    // Query disks using lsblk key-value format
    FILE *fp = popen("lsblk -d -n -P -o NAME,SIZE,MODEL,RO,RM 2>/dev/null", "r");
    if (!fp) {
        log_to_ui("Failed to scan devices: lsblk failed.\n");
        return;
    }

    char line[512];
    gboolean show_all = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_show_all_drives));

    while (fgets(line, sizeof(line), fp) && disk_count < MAX_DISKS) {
        char name[128] = {0};
        char size[128] = {0};
        char model[256] = {0};
        char ro_str[32] = {0};
        char rm_str[32] = {0};

        // Extract using strstr / sscanf style parsing
        char *p;
        if ((p = strstr(line, "NAME=\""))) sscanf(p, "NAME=\"%[^\"]", name);
        if ((p = strstr(line, "SIZE=\""))) sscanf(p, "SIZE=\"%[^\"]", size);
        if ((p = strstr(line, "MODEL=\""))) sscanf(p, "MODEL=\"%[^\"]", model);
        if ((p = strstr(line, "RO=\""))) sscanf(p, "RO=\"%[^\"]", ro_str);
        if ((p = strstr(line, "RM=\""))) sscanf(p, "RM=\"%[^\"]", rm_str);

        if (strlen(name) == 0) continue;

        // Skip loop, zram, and read-only disks
        if (strncmp(name, "loop", 4) == 0 || strncmp(name, "zram", 4) == 0) continue;
        int removable = atoi(rm_str);
        int read_only = atoi(ro_str);

        if (read_only) continue;

        // E.g. skip /dev/nvme0n1 if it's the root drive to protect it!
        if (strcmp(name, system_root_disk) == 0) continue;

        // If not showing all, skip non-removable internal drives
        if (!removable && !show_all) continue;

        // Populate disk struct
        snprintf(disks[disk_count].name, sizeof(disks[disk_count].name), "%s", name);
        snprintf(disks[disk_count].size, sizeof(disks[disk_count].size), "%s", size);
        
        if (strlen(model) > 0) {
            snprintf(disks[disk_count].model, sizeof(disks[disk_count].model), "%s", model);
        } else {
            strcpy(disks[disk_count].model, "Generic Flash Drive");
        }
        
        disks[disk_count].removable = removable;
        disks[disk_count].read_only = read_only;

        // Display string: "/dev/sdb - 16 GB [Cruzer Blade]"
        char label[512];
        snprintf(label, sizeof(label), "/dev/%s - %s [%s]%s", 
                 disks[disk_count].name, 
                 disks[disk_count].size, 
                 disks[disk_count].model, 
                 removable ? "" : " (Internal)");

        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_device), label);
        disk_count++;
    }
    pclose(fp);

    if (disk_count > 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_device), 0);
    } else {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_device), "No compatible USB drives found");
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_device), 0);
    }
}

// Background thread structure for checksum calculation
typedef struct {
    char file_path[1024];
} HashThreadData;

static gboolean idle_update_hashes(gpointer data) {
    char *result_text = (char *)data;
    
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                               GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_INFO,
                                               GTK_BUTTONS_OK,
                                               "Image Checksums");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", result_text);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    
    g_free(result_text);
    return FALSE;
}

void *calc_hashes_thread(void *arg) {
    HashThreadData *data = (HashThreadData *)arg;
    
    log_from_thread("Calculating checksums for %s...\n", data->file_path);
    
    FILE *fp = fopen(data->file_path, "rb");
    if (!fp) {
        log_from_thread("Error opening image file for checksum.\n");
        g_free(data);
        return NULL;
    }

    EVP_MD_CTX *md5_ctx = EVP_MD_CTX_new();
    EVP_MD_CTX *sha1_ctx = EVP_MD_CTX_new();
    EVP_MD_CTX *sha256_ctx = EVP_MD_CTX_new();

    EVP_DigestInit_ex(md5_ctx, EVP_md5(), NULL);
    EVP_DigestInit_ex(sha1_ctx, EVP_sha1(), NULL);
    EVP_DigestInit_ex(sha256_ctx, EVP_sha256(), NULL);

    unsigned char buffer[65536];
    size_t bytes_read;
    unsigned long long total_bytes = 0;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        EVP_DigestUpdate(md5_ctx, buffer, bytes_read);
        EVP_DigestUpdate(sha1_ctx, buffer, bytes_read);
        EVP_DigestUpdate(sha256_ctx, buffer, bytes_read);
        total_bytes += bytes_read;
    }
    
    fclose(fp);

    unsigned char md5_hash[16];
    unsigned char sha1_hash[20];
    unsigned char sha256_hash[32];
    unsigned int md5_len, sha1_len, sha256_len;

    EVP_DigestFinal_ex(md5_ctx, md5_hash, &md5_len);
    EVP_DigestFinal_ex(sha1_ctx, sha1_hash, &sha1_len);
    EVP_DigestFinal_ex(sha256_ctx, sha256_hash, &sha256_len);

    EVP_MD_CTX_free(md5_ctx);
    EVP_MD_CTX_free(sha1_ctx);
    EVP_MD_CTX_free(sha256_ctx);

    char *result = g_malloc(2048);
    char md5_str[33] = {0};
    char sha1_str[41] = {0};
    char sha256_str[65] = {0};

    for (int i = 0; i < 16; i++) sprintf(&md5_str[i * 2], "%02x", md5_hash[i]);
    for (int i = 0; i < 20; i++) sprintf(&sha1_str[i * 2], "%02x", sha1_hash[i]);
    for (int i = 0; i < 32; i++) sprintf(&sha256_str[i * 2], "%02x", sha256_hash[i]);

    snprintf(result, 2048, 
             "File: %s\n"
             "Size: %llu bytes (%g GB)\n\n"
             "MD5:    %s\n"
             "SHA-1:  %s\n"
             "SHA-256:%s\n",
             g_path_get_basename(data->file_path),
             total_bytes,
             (double)total_bytes / (1024 * 1024 * 1024),
             md5_str,
             sha1_str,
             sha256_str);

    log_from_thread("MD5: %s\n", md5_str);
    log_from_thread("SHA-1: %s\n", sha1_str);
    log_from_thread("SHA-256: %s\n", sha256_str);

    g_idle_add(idle_update_hashes, result);
    g_free(data);
    return NULL;
}

// Select ISO handler
void on_select_button_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    (void)data;
    GtkFileChooserNative *native = gtk_file_chooser_native_new("Open Bootable Image",
                                                               GTK_WINDOW(window),
                                                               GTK_FILE_CHOOSER_ACTION_OPEN,
                                                               "_Open",
                                                               "_Cancel");

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Bootable ISO/IMG Images (*.iso, *.img)");
    gtk_file_filter_add_pattern(filter, "*.iso");
    gtk_file_filter_add_pattern(filter, "*.iso.gz");
    gtk_file_filter_add_pattern(filter, "*.img");
    gtk_file_filter_add_pattern(filter, "*.img.gz");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(native), filter);

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All Files (*.*)");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(native), all_filter);

    gint res = gtk_native_dialog_run(GTK_NATIVE_DIALOG(native));
    if (res == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(native));
        snprintf(selected_iso_path, sizeof(selected_iso_path), "%s", filename);
        
        struct stat st;
        if (stat(selected_iso_path, &st) == 0) {
            selected_iso_size = st.st_size;
        }

        // Set Label
        char *base = g_path_get_basename(selected_iso_path);
        gtk_label_set_text(GTK_LABEL(lbl_iso_name), base);
        
        // Update volume label automatically if it is empty
        const char *current_label = gtk_entry_get_text(GTK_ENTRY(entry_volume_label));
        if (strlen(current_label) == 0) {
            char suggested[32] = {0};
            strncpy(suggested, base, 11); // FAT32 limit is 11 chars
            char *dot = strchr(suggested, '.');
            if (dot) *dot = '\0';
            gtk_entry_set_text(GTK_ENTRY(entry_volume_label), suggested);
        }

        log_to_ui("Selected image file: %s (%llu bytes)\n", selected_iso_path, selected_iso_size);

        // Spawn hash thread
        pthread_t hash_tid;
        HashThreadData *hdata = g_new0(HashThreadData, 1);
        snprintf(hdata->file_path, sizeof(hdata->file_path), "%s", selected_iso_path);
        pthread_create(&hash_tid, NULL, calc_hashes_thread, hdata);
        pthread_detach(hash_tid);

        g_free(base);
        g_free(filename);
    }
    g_object_unref(native);
}

// Callback to link Partition Scheme changes to Target System selection
void on_partition_scheme_changed(GtkComboBox *combo, gpointer data) {
    (void)data;
    const char *scheme = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (scheme) {
        if (strcmp(scheme, "MBR") == 0) {
            int target_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_target_system));
            if (target_idx != 1) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(combo_target_system), 1); // BIOS or UEFI-CSM
            }
        } else if (strcmp(scheme, "GPT") == 0) {
            int target_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_target_system));
            if (target_idx != 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(combo_target_system), 0); // UEFI (non CSM)
            }
        }
    }
}

// Callback to link Target System changes to Partition Scheme selection
void on_target_system_changed(GtkComboBox *combo, gpointer data) {
    (void)data;
    const char *target = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (target) {
        if (strcmp(target, "BIOS or UEFI-CSM") == 0) {
            int part_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_partition_scheme));
            if (part_idx != 1) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(combo_partition_scheme), 1); // MBR
            }
        } else if (strcmp(target, "UEFI (non CSM)") == 0) {
            int part_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_partition_scheme));
            if (part_idx != 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(combo_partition_scheme), 0); // GPT
            }
        }
    }
}

// Watch boot selection combobox to enable/disable SELECT button
void on_boot_selection_changed(GtkComboBox *combo, gpointer data) {
    (void)data;
    const char *sel = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (sel && strcmp(sel, "Disk or ISO image (Please select)") == 0) {
        gtk_widget_set_sensitive(btn_select_iso, TRUE);
    } else {
        gtk_widget_set_sensitive(btn_select_iso, FALSE);
        gtk_label_set_text(GTK_LABEL(lbl_iso_name), "No image selected");
        selected_iso_path[0] = '\0';
    }
}

// Show/Hide log window handler
void on_log_toggle_clicked(GtkToggleButton *btn, gpointer data) {
    (void)data;
    gboolean active = gtk_toggle_button_get_active(btn);
    if (active) {
        gtk_widget_show(scrolled_log);
    } else {
        gtk_widget_hide(scrolled_log);
    }
}

// Structure to pass args to burning thread
typedef struct {
    char device_path[64];
    char iso_path[1024];
    char filesystem[32];
    char partition_scheme[32];
    char volume_label[64];
    int write_iso; // 1 = copy ISO (DD), 0 = format only
} BurnThreadArgs;

static gboolean idle_finish_burn(gpointer data) {
    char *result_msg = (char *)data;
    
    // Enable controls again
    gtk_widget_set_sensitive(combo_device, TRUE);
    gtk_widget_set_sensitive(combo_boot_selection, TRUE);
    gtk_widget_set_sensitive(combo_partition_scheme, TRUE);
    gtk_widget_set_sensitive(combo_target_system, TRUE);
    gtk_widget_set_sensitive(entry_volume_label, TRUE);
    gtk_widget_set_sensitive(combo_filesystem, TRUE);
    gtk_widget_set_sensitive(combo_cluster_size, TRUE);
    gtk_widget_set_sensitive(btn_start, TRUE);
    gtk_widget_set_sensitive(btn_close, TRUE);
    
    // If selected ISO is active
    on_boot_selection_changed(GTK_COMBO_BOX(combo_boot_selection), NULL);

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                               GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_INFO,
                                               GTK_BUTTONS_OK,
                                               "Operation Completed");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", result_msg);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    update_ui_progress(1.0, "Ready", "Success!");
    
    g_free(result_msg);
    return FALSE;
}

// Background thread writing ISO or Formatting
void *burn_thread_func(void *arg) {
    BurnThreadArgs *args = (BurnThreadArgs *)arg;

    log_from_thread("==================================================\n");
    log_from_thread("Starting formatting/writing process for device: %s\n", args->device_path);
    log_from_thread("Target partition scheme: %s\n", args->partition_scheme);
    log_from_thread("Target filesystem: %s\n", args->filesystem);
    log_from_thread("Volume label: %s\n", args->volume_label);
    if (args->write_iso) {
        log_from_thread("Source ISO image: %s\n", args->iso_path);
    }
    log_from_thread("==================================================\n");

    // 1. Unmount any active partitions on the disk
    update_ui_progress(0.02, "Unmounting existing partitions...", "Busy");
    log_from_thread("Unmounting any active partitions on %s...\n", args->device_path);
    
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "umount %s* 2>/dev/null || true", args->device_path);
    if (system(cmd) != 0) {
        // Log or handle error if needed, but since it has '|| true' it always returns success
    }
    
    // Give udev some time to catch up
    sleep(1);

    // 2. Wipe existing headers (safely clear first 10MB of drive to clear partition structures)
    update_ui_progress(0.05, "Wiping partition tables...", "Busy");
    log_from_thread("Wiping partition table on %s...\n", args->device_path);
    snprintf(cmd, sizeof(cmd), "dd if=/dev/zero of=%s bs=1M count=10 oflag=sync conv=notrunc 2>&1", args->device_path);
    FILE *wipe_fp = popen(cmd, "r");
    if (wipe_fp) {
        char buf[256];
        while (fgets(buf, sizeof(buf), wipe_fp)) {
            log_from_thread("  [WIPE] %s", buf);
        }
        pclose(wipe_fp);
    }

    if (!args->write_iso) {
        // Just Partitioning and Formatting
        update_ui_progress(0.20, "Creating partitions...", "Busy");
        log_from_thread("Creating partition table using parted...\n");
        
        char label_type[16] = "msdos";
        if (strcmp(args->partition_scheme, "GPT") == 0) {
            strcpy(label_type, "gpt");
        }

        snprintf(cmd, sizeof(cmd), "parted -s %s mklabel %s mkpart primary 1MiB 100%% 2>&1", args->device_path, label_type);
        log_from_thread("Executing: %s\n", cmd);
        
        FILE *part_fp = popen(cmd, "r");
        if (part_fp) {
            char buf[256];
            while (fgets(buf, sizeof(buf), part_fp)) {
                log_from_thread("  [PARTED] %s", buf);
            }
            pclose(part_fp);
        }

        // Give kernel time to reload partition table
        sleep(2);
        
        // Find target partition, typically /dev/sdb1
        char partition_path[128];
        if (strstr(args->device_path, "nvme")) {
            snprintf(partition_path, sizeof(partition_path), "%sp1", args->device_path);
        } else {
            snprintf(partition_path, sizeof(partition_path), "%s1", args->device_path);
        }

        update_ui_progress(0.50, "Formatting filesystem...", "Busy");
        log_from_thread("Formatting partition %s to %s with label '%s'...\n", partition_path, args->filesystem, args->volume_label);

        if (strcmp(args->filesystem, "FAT32") == 0) {
            snprintf(cmd, sizeof(cmd), "mkfs.vfat -F 32 -n \"%s\" %s 2>&1", args->volume_label, partition_path);
        } else if (strcmp(args->filesystem, "NTFS") == 0) {
            snprintf(cmd, sizeof(cmd), "mkfs.ntfs -f -L \"%s\" %s 2>&1", args->volume_label, partition_path);
        } else if (strcmp(args->filesystem, "exFAT") == 0) {
            snprintf(cmd, sizeof(cmd), "mkfs.exfat -n \"%s\" %s 2>&1", args->volume_label, partition_path);
        } else {
            // ext4
            snprintf(cmd, sizeof(cmd), "mkfs.ext4 -F -L \"%s\" %s 2>&1", args->volume_label, partition_path);
        }

        log_from_thread("Executing: %s\n", cmd);
        FILE *format_fp = popen(cmd, "r");
        if (format_fp) {
            char buf[256];
            while (fgets(buf, sizeof(buf), format_fp)) {
                log_from_thread("  [MKFS] %s", buf);
            }
            pclose(format_fp);
        }

    } else {
        // Writing ISO using DD style progress copying
        log_from_thread("Starting ISO burn process...\n");
        
        struct timeval start_time, current_time;
        gettimeofday(&start_time, NULL);

        // Run dd status=progress with redirection
        snprintf(cmd, sizeof(cmd), "dd if=\"%s\" of=\"%s\" bs=4M status=progress conv=fdatasync 2>&1", args->iso_path, args->device_path);
        log_from_thread("Executing: %s\n", cmd);

        FILE *dd_fp = popen(cmd, "r");
        if (!dd_fp) {
            log_from_thread("CRITICAL ERROR: Failed to launch dd subprocess!\n");
            char *err = g_strdup("Failed to write drive: could not spawn dd.");
            g_idle_add(idle_finish_burn, err);
            g_free(args);
            return NULL;
        }

        char line[256];
        while (fgets(line, sizeof(line), dd_fp)) {
            // Log raw progress lines safely or print to terminal
            printf("dd raw: %s", line);

            unsigned long long bytes_copied = strtoull(line, NULL, 10);
            if (bytes_copied > 0 && selected_iso_size > 0) {
                double progress = (double)bytes_copied / selected_iso_size;
                if (progress > 0.99) progress = 0.99; // Preserve 100% for sync
                
                gettimeofday(&current_time, NULL);
                double elapsed = (current_time.tv_sec - start_time.tv_sec) + 
                                 (current_time.tv_usec - start_time.tv_usec) / 1000000.0;
                
                if (elapsed > 0.1) {
                    double speed_bps = bytes_copied / elapsed;
                    double speed_mb = speed_bps / (1024.0 * 1024.0);
                    
                    double remaining_bytes = selected_iso_size - bytes_copied;
                    double remaining_seconds = remaining_bytes / speed_bps;
                    
                    char speed_str[64];
                    char eta_str[64];
                    char status_str[128];
                    
                    snprintf(speed_str, sizeof(speed_str), "Speed: %.1f MB/s", speed_mb);
                    
                    int hours = (int)(remaining_seconds / 3600);
                    int minutes = (int)((remaining_seconds - hours * 3600) / 60);
                    int seconds = (int)(remaining_seconds - hours * 3600 - minutes * 60);
                    
                    if (hours > 0) {
                        snprintf(eta_str, sizeof(eta_str), "ETA: %02d:%02d:%02d", hours, minutes, seconds);
                    } else {
                        snprintf(eta_str, sizeof(eta_str), "ETA: %02d:%02d", minutes, seconds);
                    }
                    
                    snprintf(status_str, sizeof(status_str), "Writing ISO... %.1f%%", progress * 100.0);
                    update_ui_progress(progress, status_str, g_strdup_printf("%s | %s", speed_str, eta_str));
                }
            } else {
                // If it is just informational text, log it into the console textview
                log_from_thread("  [DD] %s", line);
            }
        }
        pclose(dd_fp);
    }

    // 3. Final synchronization (cache flushing)
    update_ui_progress(0.99, "Synchronizing filesystem cache (Flushing buffers)...", "Syncing");
    log_from_thread("Flushing cache buffers to complete writes safely... Please do not unplug the drive!\n");
    system("sync");
    log_from_thread("Synchronized. USB drive is now safe to unplug.\n");

    // Success popup message
    char *success_msg = g_strdup_printf("Rufus has successfully formatted and written to the USB device %s.\nYour bootable drive is ready!", args->device_path);
    g_idle_add(idle_finish_burn, success_msg);

    g_free(args);
    return NULL;
}

// Start button handler
void on_start_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    (void)data;
    int active_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_device));
    if (active_idx < 0 || active_idx >= disk_count) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                                   GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "No Target Device Selected");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), 
                                                 "Please select a valid destination USB drive first.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    DiskInfo *target_disk = &disks[active_idx];
    
    // Check if ISO boot selection requires a file
    const char *boot_sel = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_boot_selection));
    int write_iso = 0;
    if (boot_sel && strcmp(boot_sel, "Disk or ISO image (Please select)") == 0) {
        if (strlen(selected_iso_path) == 0) {
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                                       GTK_DIALOG_DESTROY_WITH_PARENT,
                                                       GTK_MESSAGE_ERROR,
                                                       GTK_BUTTONS_OK,
                                                       "No Bootable ISO Selected");
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), 
                                                     "You have selected 'Disk or ISO image' as the boot selection, but have not chosen a file. Please click 'SELECT' and choose an ISO first.");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            return;
        }
        write_iso = 1;
    }

    // Safety warning popup
    char warning_text[512];
    snprintf(warning_text, sizeof(warning_text), 
             "WARNING: ALL DATA ON DEVICE '/dev/%s' WILL BE DESTROYED!\n"
             "Disk Model: %s\n"
             "Capacity: %s\n\n"
             "To continue with this operation, click OK. To quit click Cancel.",
             target_disk->name, target_disk->model, target_disk->size);

    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                               GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_WARNING,
                                               GTK_BUTTONS_OK_CANCEL,
                                               "WARNING: DESTROYING DATA!");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", warning_text);
    
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_OK) {
        // Disable UI controls to avoid race issues
        gtk_widget_set_sensitive(combo_device, FALSE);
        gtk_widget_set_sensitive(combo_boot_selection, FALSE);
        gtk_widget_set_sensitive(combo_partition_scheme, FALSE);
        gtk_widget_set_sensitive(combo_target_system, FALSE);
        gtk_widget_set_sensitive(entry_volume_label, FALSE);
        gtk_widget_set_sensitive(combo_filesystem, FALSE);
        gtk_widget_set_sensitive(combo_cluster_size, FALSE);
        gtk_widget_set_sensitive(btn_select_iso, FALSE);
        gtk_widget_set_sensitive(btn_start, FALSE);
        gtk_widget_set_sensitive(btn_close, FALSE);

        // Prep thread arguments
        BurnThreadArgs *args = g_new0(BurnThreadArgs, 1);
        snprintf(args->device_path, sizeof(args->device_path), "/dev/%s", target_disk->name);
        snprintf(args->iso_path, sizeof(args->iso_path), "%s", selected_iso_path);
        
        const char *fs = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_filesystem));
        if (fs) snprintf(args->filesystem, sizeof(args->filesystem), "%s", fs);
        
        const char *part = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_partition_scheme));
        if (part) snprintf(args->partition_scheme, sizeof(args->partition_scheme), "%s", part);
        
        const char *label = gtk_entry_get_text(GTK_ENTRY(entry_volume_label));
        if (label && strlen(label) > 0) {
            snprintf(args->volume_label, sizeof(args->volume_label), "%s", label);
        } else {
            strcpy(args->volume_label, "RUFUS");
        }

        args->write_iso = write_iso;

        // Spawn Burn Background thread
        pthread_t burn_tid;
        pthread_create(&burn_tid, NULL, burn_thread_func, args);
        pthread_detach(burn_tid);
    }
}

// GUI builder with custom styling
int main(int argc, char *argv[]) {
    // Suppress dconf warnings by using memory backend
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);

    // Force use of portal for native system file chooser instead of GTK fallback
    g_setenv("GTK_USE_PORTAL", "1", TRUE);

    // Disable event grabs to fix combo box popup menus closing on click
    // (especially when running as root or under Wayland)
    g_setenv("GDK_DISABLE_GRAB", "1", TRUE);

    // Suppress GNOME accessibility bridge warnings
    g_setenv("NO_AT_BRIDGE", "1", TRUE);

    // Bypass Wayland pointer/popup grab bugs when running as root by forcing X11 (Xwayland) backend
    if (getenv("WAYLAND_DISPLAY") != NULL) {
        g_setenv("GDK_BACKEND", "x11", TRUE);
    }

    // Initialize GTK first so we can safely show warning dialogs
    gtk_init(&argc, &argv);

    GdkDisplay *gdisplay = gdk_display_get_default();
    const char *backend_type = "unknown";
#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(gdisplay)) {
        backend_type = "Wayland";
    }
#endif
#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(gdisplay)) {
        backend_type = "X11";
    }
#endif
    printf("GDK windowing backend: %s\n", backend_type);

    // Check for root privilege
    if (geteuid() != 0) {
        if (getenv("PKEXEC_UID") != NULL) {
            // We're already under pkexec but still not root — pkexec/auth failed
            GtkWidget *err_dialog = gtk_message_dialog_new(NULL,
                GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_ERROR,
                GTK_BUTTONS_OK,
                "Privilege Escalation Failed");
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(err_dialog),
                "Rufus needs root privileges to write raw block devices.\n"
                "Please run the application as root (e.g. 'sudo rufus-linux').");
            gtk_dialog_run(GTK_DIALOG(err_dialog));
            gtk_widget_destroy(err_dialog);
            return 1;
        }

        // Try to self-elevate via pkexec
        char self_path[4096];
        ssize_t rlen = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
        if (rlen > 0 && rlen < (ssize_t)sizeof(self_path)) {
            self_path[rlen] = '\0';
            char *argv[] = { "pkexec", self_path, NULL };
            GError *error = NULL;
            g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                NULL, NULL, NULL, &error);
            if (error == NULL) {
                // pkexec launched — it will ask for password via polkit
                return 0;
            }
            g_error_free(error);
        }

        // pkexec not available or failed, show error
        GtkWidget *err_dialog = gtk_message_dialog_new(NULL,
            GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "Privilege Escalation Required");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(err_dialog),
            "Rufus needs root privileges to write raw block devices.\n"
            "Please run the application as root (e.g. 'sudo rufus-linux' "
            "or using 'pkexec rufus-linux').");
        gtk_dialog_run(GTK_DIALOG(err_dialog));
        gtk_widget_destroy(err_dialog);
        return 1;
    }

    // Apply custom modern dark-theme styles via CSS provider
    GtkCssProvider *css_provider = gtk_css_provider_new();
    const char *theme_css = 
        "window {\n"
        "    background-color: #11111b;\n"
        "    color: #cdd6f4;\n"
        "    font-family: 'Inter', 'Segoe UI', 'Liberation Sans', sans-serif;\n"
        "}\n"
        "frame {\n"
        "    border: 1px solid #313244;\n"
        "    border-radius: 8px;\n"
        "    background-color: #1e1e2e;\n"
        "    margin: 6px 12px;\n"
        "    padding: 10px;\n"
        "}\n"
        "frame > label {\n"
        "    font-weight: bold;\n"
        "    color: #89b4fa;\n"
        "    font-size: 13px;\n"
        "    margin-bottom: 4px;\n"
        "}\n"
        "label {\n"
        "    color: #cdd6f4;\n"
        "    font-size: 13px;\n"
        "    font-weight: 500;\n"
        "}\n"
        "combobox, entry {\n"
        "    background-color: #313244;\n"
        "    color: #cdd6f4;\n"
        "    border: 1px solid #45475a;\n"
        "    border-radius: 6px;\n"
        "    padding: 4px 8px;\n"
        "}\n"
        "combobox:hover, entry:focus {\n"
        "    border-color: #89b4fa;\n"
        "}\n"
        "combobox menu, combobox menuitem, menu, menuitem, modelbutton {\n"
        "    background-color: #1e1e2e;\n"
        "    color: #cdd6f4;\n"
        "    border: 1px solid #313244;\n"
        "}\n"
        "combobox menuitem:hover, menuitem:hover, modelbutton:hover {\n"
        "    background-color: #313244;\n"
        "    color: #89b4fa;\n"
        "}\n"
        "button {\n"
        "    background: linear-gradient(135deg, #89b4fa, #b4befe);\n"
        "    color: #11111b;\n"
        "    font-weight: bold;\n"
        "    border-radius: 6px;\n"
        "    border: none;\n"
        "    padding: 6px 12px;\n"
        "    transition: all 0.3s ease;\n"
        "}\n"
        "button:hover {\n"
        "    background: linear-gradient(135deg, #b4befe, #89b4fa);\n"
        "    box-shadow: 0 0 8px rgba(137, 180, 250, 0.5);\n"
        "}\n"
        "progressbar trough {\n"
        "    background-color: #313244;\n"
        "    border-radius: 6px;\n"
        "    min-height: 18px;\n"
        "}\n"
        "progressbar progress {\n"
        "    background: linear-gradient(90deg, #89b4fa, #a6e3a1);\n"
        "    border-radius: 6px;\n"
        "}\n"
        "progressbar text {\n"
        "    color: #ffffff;\n"
        "    font-weight: bold;\n"
        "    font-size: 11px;\n"
        "    text-shadow: 0 1px 3px rgba(0, 0, 0, 0.8);\n"
        "}\n"
        "textview, textview text {\n"
        "    background-color: #11111b;\n"
        "    color: #a6e3a1;\n"
        "    font-family: 'Fira Code', 'Monospace', monospace;\n"
        "    font-size: 11px;\n"
        "}\n"
        "scrollbar trough {\n"
        "    background-color: #11111b;\n"
        "    border-radius: 4px;\n"
        "}\n"
        "scrollbar slider {\n"
        "    background-color: #45475a;\n"
        "    border-radius: 4px;\n"
        "}\n"
        "scrollbar slider:hover {\n"
        "    background-color: #585b70;\n"
        "}\n"
        "#lbl-status {\n"
        "    color: #a6e3a1;\n"
        "    font-weight: bold;\n"
        "}\n"
        "#lbl-speed-eta {\n"
        "    color: #89b4fa;\n"
        "    font-weight: bold;\n"
        "    font-family: 'Fira Code', 'Monospace', monospace;\n"
        "}\n"
        ".btn-accent {\n"
        "    background: linear-gradient(135deg, #a6e3a1, #94e2d5);\n"
        "}\n"
        ".btn-accent:hover {\n"
        "    background: linear-gradient(135deg, #94e2d5, #a6e3a1);\n"
        "    box-shadow: 0 0 8px rgba(166, 227, 161, 0.5);\n"
        "}\n"
        ".btn-gray {\n"
        "    background: #313244;\n"
        "    color: #cdd6f4;\n"
        "    border: 1px solid #45475a;\n"
        "}\n"
        ".btn-gray:hover {\n"
        "    background: #45475a;\n"
        "}\n"
        ".app-header {\n"
        "    background: linear-gradient(90deg, #181825, #313244);\n"
        "    padding: 12px;\n"
        "    border-bottom: 2px solid #89b4fa;\n"
        "    margin-bottom: 8px;\n"
        "}\n";

    gtk_css_provider_load_from_data(css_provider, theme_css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                               GTK_STYLE_PROVIDER(css_provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_SETTINGS);

    // Main window setup - widescreen two-column dashboard layout (keeps all elements visible without scrolls)
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), APP_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(window), 850, 490);
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);

    // Premium App Header
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_name(header_box, "header");
    gtk_style_context_add_class(gtk_widget_get_style_context(header_box), "app-header");
    
    GtkWidget *lbl_header_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_header_title), "<span font='16' weight='bold' color='#89b4fa'>Rufus</span> <span font='11' color='#a6adc8'>v" APP_VERSION " (Linux)</span>");
    gtk_box_pack_start(GTK_BOX(header_box), lbl_header_title, FALSE, FALSE, 10);
    gtk_box_pack_start(GTK_BOX(main_vbox), header_box, FALSE, FALSE, 0);

    // Two-Column Horizontal Container
    GtkWidget *body_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(body_hbox), 6);
    gtk_box_pack_start(GTK_BOX(main_vbox), body_hbox, TRUE, TRUE, 0);

    // Left Column: Drive Properties
    GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(body_hbox), left_vbox, TRUE, TRUE, 0);

    // Right Column: Format Options & Status
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(body_hbox), right_vbox, TRUE, TRUE, 0);

    // Frame 1: Drive Properties (placed on Left Column)
    GtkWidget *frame_drive = gtk_frame_new("Drive Properties");
    gtk_box_pack_start(GTK_BOX(left_vbox), frame_drive, FALSE, FALSE, 0);

    GtkWidget *grid_drive = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_drive), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid_drive), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid_drive), 8);
    gtk_container_add(GTK_CONTAINER(frame_drive), grid_drive);

    // Device
    GtkWidget *lbl_device = gtk_label_new("Device");
    gtk_widget_set_halign(lbl_device, GTK_ALIGN_START);
    combo_device = gtk_combo_box_text_new();
    
    GtkWidget *box_device_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(box_device_controls), combo_device, TRUE, TRUE, 0);
    
    GtkWidget *btn_refresh = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_refresh), "btn-gray");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(refresh_devices), NULL);
    gtk_box_pack_start(GTK_BOX(box_device_controls), btn_refresh, FALSE, FALSE, 0);

    gtk_grid_attach(GTK_GRID(grid_drive), lbl_device, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_drive), box_device_controls, 1, 0, 1, 1);

    // Advanced Drive Properties Panel (GtkExpander)
    GtkWidget *expander_drive = gtk_expander_new("Show advanced drive properties");
    GtkWidget *expander_drive_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(expander_drive_vbox), 6);
    gtk_container_add(GTK_CONTAINER(expander_drive), expander_drive_vbox);

    check_show_all_drives = gtk_check_button_new_with_label("List USB Hard Drives (Use with caution)");
    g_signal_connect(check_show_all_drives, "toggled", G_CALLBACK(refresh_devices), NULL);
    gtk_box_pack_start(GTK_BOX(expander_drive_vbox), check_show_all_drives, FALSE, FALSE, 0);

    GtkWidget *check_old_bios = gtk_check_button_new_with_label("Add fixes for old BIOSes (extra partition, alignment)");
    gtk_box_pack_start(GTK_BOX(expander_drive_vbox), check_old_bios, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(left_vbox), expander_drive, FALSE, FALSE, 6);

    // Advanced Format Options Panel (GtkExpander) - Moved to Left Column under Drive Expanders
    GtkWidget *expander_format = gtk_expander_new("Show advanced format options");
    GtkWidget *expander_format_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(expander_format_vbox), 6);
    gtk_container_add(GTK_CONTAINER(expander_format), expander_format_vbox);

    GtkWidget *check_quick_format = gtk_check_button_new_with_label("Quick format");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_quick_format), TRUE);
    gtk_box_pack_start(GTK_BOX(expander_format_vbox), check_quick_format, FALSE, FALSE, 0);

    GtkWidget *check_extended_labels = gtk_check_button_new_with_label("Create extended label and icon files");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_extended_labels), TRUE);
    gtk_box_pack_start(GTK_BOX(expander_format_vbox), check_extended_labels, FALSE, FALSE, 0);

    GtkWidget *box_bad_blocks = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *check_bad_blocks = gtk_check_button_new_with_label("Check device for bad blocks  ");
    GtkWidget *combo_bad_blocks = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_bad_blocks), "1 Pass");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_bad_blocks), "2 Passes");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_bad_blocks), "3 Passes");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_bad_blocks), "4 Passes");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_bad_blocks), 0);
    
    gtk_box_pack_start(GTK_BOX(box_bad_blocks), check_bad_blocks, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box_bad_blocks), combo_bad_blocks, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(expander_format_vbox), box_bad_blocks, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(left_vbox), expander_format, FALSE, FALSE, 6);

    // Boot Selection
    GtkWidget *lbl_boot = gtk_label_new("Boot selection");
    gtk_widget_set_halign(lbl_boot, GTK_ALIGN_START);
    combo_boot_selection = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_boot_selection), "Disk or ISO image (Please select)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_boot_selection), "Non bootable");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_boot_selection), "FreeDOS");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_boot_selection), 0);
    g_signal_connect(combo_boot_selection, "changed", G_CALLBACK(on_boot_selection_changed), NULL);

    btn_select_iso = gtk_button_new_with_label("SELECT");
    g_signal_connect(btn_select_iso, "clicked", G_CALLBACK(on_select_button_clicked), NULL);

    GtkWidget *box_boot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(box_boot), combo_boot_selection, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box_boot), btn_select_iso, FALSE, FALSE, 0);

    gtk_grid_attach(GTK_GRID(grid_drive), lbl_boot, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_drive), box_boot, 1, 2, 1, 1);

    // ISO Label Display
    GtkWidget *lbl_iso_title = gtk_label_new("Selected file");
    gtk_widget_set_halign(lbl_iso_title, GTK_ALIGN_START);
    lbl_iso_name = gtk_label_new("No image selected");
    gtk_widget_set_halign(lbl_iso_name, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_iso_name), PANGO_ELLIPSIZE_END);
    gtk_grid_attach(GTK_GRID(grid_drive), lbl_iso_title, 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_drive), lbl_iso_name, 1, 3, 1, 1);

    // Partition scheme
    GtkWidget *lbl_partition = gtk_label_new("Partition scheme");
    gtk_widget_set_halign(lbl_partition, GTK_ALIGN_START);
    combo_partition_scheme = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_partition_scheme), "GPT");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_partition_scheme), "MBR");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_partition_scheme), 0);

    gtk_grid_attach(GTK_GRID(grid_drive), lbl_partition, 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_drive), combo_partition_scheme, 1, 4, 1, 1);

    // Target system
    GtkWidget *lbl_target = gtk_label_new("Target system");
    gtk_widget_set_halign(lbl_target, GTK_ALIGN_START);
    combo_target_system = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_target_system), "UEFI (non CSM)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_target_system), "BIOS or UEFI-CSM");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_target_system), 0);

    gtk_grid_attach(GTK_GRID(grid_drive), lbl_target, 0, 5, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_drive), combo_target_system, 1, 5, 1, 1);

    // Bind interactive scheme logic callbacks to prevent conflicts
    g_signal_connect(combo_partition_scheme, "changed", G_CALLBACK(on_partition_scheme_changed), NULL);
    g_signal_connect(combo_target_system, "changed", G_CALLBACK(on_target_system_changed), NULL);

    // Frame 2: Format Options (placed on Right Column)
    GtkWidget *frame_format = gtk_frame_new("Format Options");
    gtk_box_pack_start(GTK_BOX(right_vbox), frame_format, FALSE, FALSE, 0);

    GtkWidget *grid_format = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_format), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid_format), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid_format), 8);
    gtk_container_add(GTK_CONTAINER(frame_format), grid_format);

    // Volume Label
    GtkWidget *lbl_volume = gtk_label_new("Volume label");
    gtk_widget_set_halign(lbl_volume, GTK_ALIGN_START);
    entry_volume_label = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_volume_label), "RUFUS");

    gtk_grid_attach(GTK_GRID(grid_format), lbl_volume, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_format), entry_volume_label, 1, 0, 1, 1);

    // File System
    GtkWidget *lbl_fs = gtk_label_new("File system");
    gtk_widget_set_halign(lbl_fs, GTK_ALIGN_START);
    combo_filesystem = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_filesystem), "FAT32");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_filesystem), "NTFS");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_filesystem), "exFAT");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_filesystem), "ext4");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_filesystem), 0);

    gtk_grid_attach(GTK_GRID(grid_format), lbl_fs, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_format), combo_filesystem, 1, 1, 1, 1);

    // Cluster Size
    GtkWidget *lbl_cluster = gtk_label_new("Cluster size");
    gtk_widget_set_halign(lbl_cluster, GTK_ALIGN_START);
    combo_cluster_size = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cluster_size), "Default (16 kilobytes)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cluster_size), "4096 bytes");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cluster_size), "8192 bytes");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cluster_size), "32 kilobytes");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_cluster_size), 0);

    gtk_grid_attach(GTK_GRID(grid_format), lbl_cluster, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_format), combo_cluster_size, 1, 2, 1, 1);

    // Advanced Format Options removed from here and moved to left column

    // Frame 3: Status & Progress (placed on Right Column)
    GtkWidget *frame_status = gtk_frame_new("Status");
    gtk_box_pack_start(GTK_BOX(right_vbox), frame_status, TRUE, TRUE, 0);

    GtkWidget *vbox_status = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox_status), 8);
    gtk_container_add(GTK_CONTAINER(frame_status), vbox_status);

    progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress_bar), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox_status), progress_bar, FALSE, FALSE, 0);

    GtkWidget *box_labels = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    lbl_status = gtk_label_new("Ready");
    gtk_widget_set_name(lbl_status, "lbl-status");
    gtk_widget_set_halign(lbl_status, GTK_ALIGN_START);
    lbl_speed_eta = gtk_label_new("");
    gtk_widget_set_name(lbl_speed_eta, "lbl-speed-eta");
    gtk_widget_set_halign(lbl_speed_eta, GTK_ALIGN_END);

    gtk_box_pack_start(GTK_BOX(box_labels), lbl_status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box_labels), lbl_speed_eta, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_status), box_labels, FALSE, FALSE, 0);

    // Console Log Terminal Panel
    scrolled_log = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_log), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scrolled_log, -1, 120); // Maintain a compact fixed height for logs to fit everything on screen
    
    text_log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_log), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_log), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_log), GTK_WRAP_WORD_CHAR);
    log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_log));

    gtk_container_add(GTK_CONTAINER(scrolled_log), text_log);
    gtk_box_pack_start(GTK_BOX(vbox_status), scrolled_log, TRUE, TRUE, 4);
    
    // Log drawer hidden by default
    // gtk_widget_show_all(scrolled_log);

    // Control Buttons Box at Bottom
    GtkWidget *box_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box_controls), 12);
    gtk_widget_set_halign(box_controls, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(main_vbox), box_controls, FALSE, FALSE, 0);

    btn_log_toggle = gtk_toggle_button_new_with_label("Log Drawer");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_log_toggle), TRUE); // Visible by default
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_log_toggle), "btn-gray");
    g_signal_connect(btn_log_toggle, "clicked", G_CALLBACK(on_log_toggle_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box_controls), btn_log_toggle, FALSE, FALSE, 0);

    btn_start = gtk_button_new_with_label("START");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_start), "btn-accent");
    g_signal_connect(btn_start, "clicked", G_CALLBACK(on_start_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box_controls), btn_start, FALSE, FALSE, 0);

    btn_close = gtk_button_new_with_label("CLOSE");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_close), "btn-gray");
    g_signal_connect(btn_close, "clicked", G_CALLBACK(gtk_main_quit), NULL);
    gtk_box_pack_start(GTK_BOX(box_controls), btn_close, FALSE, FALSE, 0);

    // Trigger initial device scan
    refresh_devices();

    log_to_ui("Rufus Arch Linux Port v" APP_VERSION " initialized.\n");
    log_to_ui("System block devices scanned. Safety locks active.\n");

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}
