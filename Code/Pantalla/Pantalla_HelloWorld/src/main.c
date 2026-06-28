#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "main";

// ---------------------------------------------------------------------
// CONFIGURACIÓN DE PINES SEGUROS (Mapeo probado libre de conflicto)
// ---------------------------------------------------------------------
#define LCD_RST_GPIO    27
#define LCD_CS_GPIO     15
#define LCD_DC_GPIO     21  // RS / Data-Command
#define LCD_WR_GPIO     22
// NOTA: El pin LCD_RD de la pantalla debe ir conectado a 3.3V físicos fijos.

#define LCD_D0_GPIO     32
#define LCD_D1_GPIO     33
#define LCD_D2_GPIO     25
#define LCD_D3_GPIO     26
#define LCD_D4_GPIO     2
#define LCD_D5_GPIO     4
#define LCD_D6_GPIO     18
#define LCD_D7_GPIO     19

// Resolución de la pantalla
#define LCD_H_RES       240
#define LCD_V_RES       320

// Tamaño del buffer de dibujo para LVGL (10 líneas de escaneo)
#define LVGL_BUFFER_SIZE (LCD_H_RES * 10)

// ---------------------------------------------------------------------
// MANEJADORES DE LVGL Y PANTALLA
// ---------------------------------------------------------------------
static lv_disp_draw_buf_t disp_buf;
static lv_color_t buf1[LVGL_BUFFER_SIZE];
static lv_disp_drv_t disp_drv;

// Rutina de incremento de tiempo para el motor de LVGL (1 ms)
static void lvgl_tick_cb(void *arg) {
    lv_tick_inc(1);
}

// Función intermedia que usa LVGL para avisar que terminó de enviar pixeles
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

// Callback de LVGL para volcar los pixeles en el área de la pantalla
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    
    // Enviar el bloque de color indexado al controlador de la pantalla
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

void app_main(void) {
    ESP_LOGI(TAG, "Iniciando sistema de visualización seguro...");

    // 1. INICIALIZAR EL MOTOR GRÁFICO LVGL
    lv_init();

    // 2. CONFIGURACIÓN DEL BUS PARALELO INTEL 8080
    ESP_LOGI(TAG, "Configurando bus paralelo I8080 de 8 bits...");
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = LCD_DC_GPIO,
        .wr_gpio_num = LCD_WR_GPIO,
        .data_gpio_nums = {
            LCD_D0_GPIO, LCD_D1_GPIO, LCD_D2_GPIO, LCD_D3_GPIO,
            LCD_D4_GPIO, LCD_D5_GPIO, LCD_D6_GPIO, LCD_D7_GPIO
        },
        .bus_width = 8,
        .max_transfer_bytes = LVGL_BUFFER_SIZE * sizeof(lv_color_t)
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

    // 3. CONFIGURACIÓN DEL DISPOSITIVO LOGÍSICO DEL PANEL (I/O)
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = LCD_CS_GPIO,
        .pclk_hz = 2 * 1000 * 1000, // 2 MHz para máxima estabilidad con los acopladores del shield
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .user_ctx = &disp_drv,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_data_level = 1,
        },
        .flags = {
            .cs_active_high = 0,
            .reverse_color_bits = 0,
            .swap_color_bytes = 0, 
            .pclk_active_neg = 0,
            .pclk_idle_low = 0,
        }
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle));

    // 4. CONFIGURACIÓN FÍSICA DEL DRIVER DE LA PANTALLA
    ESP_LOGI(TAG, "Instalando driver de inicialización del panel...");
    esp_lcd_panel_handle_t panel_handle = NULL;
    
    // Inicializamos la estructura limpia con ceros para evitar problemas de miembros faltantes
    esp_lcd_panel_dev_config_t panel_config = { 0 }; 
    panel_config.reset_gpio_num = LCD_RST_GPIO;
    panel_config.bits_per_pixel = 16;
    
    // Forzamos la inicialización genérica (compatible con ST7789 e ILI9341 paralelos)
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    
    // Reset físico del chip controlador
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    // Inicializar registros internos del panel
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    // Encender la visualización del panel
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    
    // Fuerza al controlador a escribir de izquierda a derecha (Espejo Horizontal = true)
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    // 5. CONFIGURACIÓN DE LOS BUFFERS DE REFRESCO DE LVGL
    ESP_LOGI(TAG, "Asignando buffers de memoria para LVGL...");
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, LVGL_BUFFER_SIZE);

    // Registrar el driver de la pantalla en LVGL
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_drv_register(&disp_drv);

    // 6. CONFIGURAR EL TEMPORIZADOR DEL SISTEMA PARA TICK DE LVGL
    ESP_LOGI(TAG, "Inicializando temporizador de ticks (1 ms)...");
    const esp_timer_create_args_t tick_timer_args = {
        .callback = &lvgl_tick_cb,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 1000)); // 1 ms

    // 7. INTERFAZ GRÁFICA DE PRUEBA
    ESP_LOGI(TAG, "Creando interfaz gráfica básica de prueba...");
    lv_obj_t *scr = lv_scr_act();
    
    // Crear una etiqueta de texto centrada en la pantalla
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Primera prueba para el proyecto\n de sistemas embebidos");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    // 8. BUCLE PRINCIPAL DE EJECUCIÓN
    ESP_LOGI(TAG, "Sistema listo. Ejecutando tarea repetitiva de LVGL...");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_timer_handler();
    }
}