#include "stm32f1xx.h"
#include "FreeRTOS.h"
#include "task.h"

#include "ubik/common/timing.h"
#include "ubik/logging/logging.h"
#include "ubik/logging/stats.h"
#include "system_monitor.h"

#include "localization/odometry.h"
#include "movement/regulator.h"
#include "movement/controller.h"
#include "movement/correction.h"
#include "common/distance_sensors.h"
#include "maze/maze.h"
#include "maze/maze_definitions.h"


void run();
extern "C" void extern_main(void) { run(); }

// void gather_distance_sensors_plot_data(float distance_to_move, float distance_resolution) {
//     float distance_moved = 0;
//     float vel_lin = 0.05;
//     float acc_lin = 0.20;
//
//     // measure
//     auto readings = distance_sensors::read(spi::gpio::DISTANCE_SENSORS[1] | spi::gpio::DISTANCE_SENSORS[4]);
//     logging::printf(30, "%f %d %d\n", distance_moved, readings.sensor[1], readings.sensor[4]);
//
//     while (distance_moved < distance_to_move) {
//         // move
//         movement::controller::move_line(-distance_resolution, vel_lin, acc_lin, vel_lin);
//         distance_moved += distance_resolution;
//
//         // measure
//         readings = distance_sensors::read(spi::gpio::DISTANCE_SENSORS[1] | spi::gpio::DISTANCE_SENSORS[4]);
//         logging::printf(30, "%f %d %d\n", distance_moved, readings.sensor[1], readings.sensor[4]);
//     }
//     // stop gracefully
//     movement::controller::move_line(-distance_resolution, vel_lin, acc_lin, 0);
// }

void show_distance_sensors_until_button() {
    system_monitor::lock_button();
    while (! system_monitor::wait_for_button_press(pdMS_TO_TICKS(100))) {
        auto readings = distance_sensors::read(spi::gpio::DISTANCE_SENSORS_ALL());
        logging::printf(100, "[sensors] %4d %4d %4d %4d %4d %4d\n",
                readings.sensor[0],
                readings.sensor[1],
                readings.sensor[2],
                readings.sensor[3],
                readings.sensor[4],
                readings.sensor[5]);
    }
    system_monitor::unlock_button();
}

void maze_solver() {
    using namespace system_monitor;
    namespace ctrl = movement::controller;

    constexpr int MAX_MAZE_SIZE = 16;

    vTaskDelay(10);
    lock_button();
    const int maze_size = select_with_wheels_int(4, {1, MAX_MAZE_SIZE}, 2*MAX_MAZE_SIZE, "[maze] select size =");

    maze::Cell cells[maze_size * maze_size];
    StaticStack<maze::Position, MAX_MAZE_SIZE * MAX_MAZE_SIZE> maze_stack;
    maze::Maze maze(maze_size, maze_size, cells, maze_stack, maze::START_POSITION);

    logging::printf(80, "[maze] Size of maze: %d (size of data structures: %d)\n",
            maze_size, maze_size * maze_size * sizeof(maze::Cell) + sizeof(maze_stack) + sizeof(maze));
    unlock_button();

    while (1) {
        lock_button();
        spi::gpio::update_pins(spi::gpio::LED_BLUE, 0);
        // auto goal_pos = maze::TargetPosition(maze_size-1, maze_size-1);
        auto goal_pos = maze::TargetPosition(
                select_with_wheels_int(maze_size/2, {0, maze_size-1}, 2*maze_size, "[maze] target.x ="),
                select_with_wheels_int(maze_size/2, {0, maze_size-1}, 2*maze_size, "[maze] target.y =")
                );
        logging::printf(100, "[maze] Moving from (%d, %d) to (%.1f, %.1f)\n[maze] Start?\n",
                maze.position().x, maze.position().y, double(goal_pos.x), double(goal_pos.y));
        spi::gpio::update_pins(0, spi::gpio::LED_BLUE);
        wait_for_button_press(portMAX_DELAY);
        unlock_button();

        localization::odometry::set_current_position({0, 0, PI/2});
        vTaskDelay(pdMS_TO_TICKS(2000));
        bool success = maze.go_to(goal_pos);
        logging::printf(100, "[maze] %s Current position (%d, %d)\n",
                success ? "Finished successfully." : "Could not finish the maze!",
                maze.position().x, maze.position().y);

        logging::printf(100, "[maze] Moving from (%d, %d) to (%.1f, %.1f)\n",
                maze.position().x, maze.position().y, double(goal_pos.x), double(goal_pos.y));
        if (success)
            success = maze.go_to(maze::START_POSITION);
        logging::printf(100, "[maze] %s Current position (%d, %d)\n",
                success ? "Finished successfully." : "Could not finish the maze!",
                maze.position().x, maze.position().y);
    }
}

void main_task(void *) {
    vTaskDelay(300);

    // gather_distance_sensors_plot_data(0.50, 0.005);

    // show_distance_sensors_until_button();
    // maze_solver();

    // movement::correction::side_walls::calibrate();
    // logging::printf(50, "Calibration complete.\n");
    // system_monitor::lock_button();
    // system_monitor::wait_for_button_press(portMAX_DELAY);
    // system_monitor::unlock_button();
    //
    // movement::correction::side_walls::set_enabled(true);
    // movement::controller::move_line(0.30, 0.20, 0.10);

    logging::printf(20, "Start?\n");
    system_monitor::lock_button();
    system_monitor::wait_for_button_press(portMAX_DELAY);
    system_monitor::unlock_button();

    movement::controller::move(new movement::Line(0.5, 0.3, 0.1, 0));
    movement::controller::move(new movement::Rotate(PI, PI, PI, 0));
    movement::controller::move(new movement::Line(0.5, 0.3, 0.1, 0));
    movement::controller::move(new movement::Arc({ 0.5, PI }, 0.3, 0.1, 0));



    while(1) {
        vTaskDelay(portMAX_DELAY);
    }
}


void run() {
    logging::printf_blocking(100, "\n===========================================\n");
    logging::printf_blocking(100, "Initialising system...\n");

    /*** Initialize modules ***************************************************/
    // this mainly initializes all the peripheral devices
    // and allocates all the required queues and semaphores
    // TODO: create some tree-like hierarchy of initializations, as this gets unwieldy

    spi::initialise(); // encoders & gpio expander
    distance_sensors::initialise(); // distance sensors' ADC
    system_monitor::initialise(); // battery and regulation control
    localization::odometry::initialise(); // encoders odomoetry
    movement::motors::initialise(); // motor control
    movement::regulator::initialise(); // PID regulator

    /*** Create FreeRTOS tasks ************************************************/

    bool all_created = true;
    // auto create_task = [&all_created] (auto ...args) {
    //     auto result = xTaskCreate(args...);
    //     all_created = all_created && result == pdPASS;
    // };
    auto create_task = [&all_created]
        (auto name, auto priority, auto func, auto stack_size, auto param)
        {
            auto result = xTaskCreate(func, name, stack_size, param, priority, nullptr);
            all_created = all_created && result == pdPASS;
        };

    create_task("SysMonitor", 4, system_monitor::system_monitor_task,
            configMINIMAL_STACK_SIZE * 2, nullptr);
    create_task("Regulator", 5, movement::regulator::regulation_task,
            configMINIMAL_STACK_SIZE * 2, nullptr);
    create_task("Controller", 3, movement::controller::controller_task,
            configMINIMAL_STACK_SIZE * 2, nullptr);
    // create_task("Stats", 1, logging::stats_monitor_task,
    //         configMINIMAL_STACK_SIZE * 2, reinterpret_cast<void *>(pdMS_TO_TICKS(10*1000)));

    create_task("Main", 2, main_task,
            configMINIMAL_STACK_SIZE * 6, nullptr);

    // create_task("Setter", 2, set_target_position_task,
    //         configMINIMAL_STACK_SIZE * 3, nullptr);
    // create_task("Distance", 3, distance_sensors_task,
    //         configMINIMAL_STACK_SIZE * 2, nullptr);

    configASSERT(all_created);

    /*** Print debug memory debug information *********************************/

    // this allows to optimize heap size that we assigned in FreeRTOSConfig.h
    // (more heap can be needed if anything is created dynamically later)
    size_t heap_size_remaining = xPortGetFreeHeapSize();
    logging::printf_blocking(60, "Remaining heap size = %u KB (%u B)\n",
            heap_size_remaining / (1 << 10), heap_size_remaining);

    /*** Start FreeRTOS scheduler *********************************************/

    logging::printf_blocking(100, "Starting scheduler...\n");

    // start RTOS event loop
    // IMPORTANT NOTE! this resets stack pointer, so all variables declared in
    // this scope will be overwritten - if needed, declare them globally or
    // allocate dynamically
    vTaskStartScheduler();

    // execution should never reach here, if it did, then something went wrong
    configASSERT(0);
}
