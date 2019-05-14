#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#define PIN     22  //pin no 15?
#define NAME    "pin"

static int __init performancetest_init(void)
{
    int ret = 0;

    printk(KERN_INFO "performancetest init\n");
    
    ret = gpio_request(PIN, NAME);
    ret = gpio_direction_output(PIN, 1);
    msleep(2);
    gpio_set_value(PIN, 0);
    msleep(2);
    gpio_set_value(PIN, 1); //delta zwischen zwei 1 (MiniDSO) -> 60ms
    msleep(2);
    gpio_set_value(PIN, 0); //delta zwischen 1 und 0 (MiniDSO) -> 30,4ms (2x) 
    msleep(2);
    gpio_set_value(PIN, 1); //delta zwischen 0 und 1 (MiniDSO) -> 29,6ms, 30,4ms -> Ergebnis auch bei mehreren Durchläufen gleich
    msleep(2);
    gpio_set_value(PIN, 0);
    msleep(2);
    gpio_set_value(PIN, 1);

    return ret;
}

static void __exit performancetest_exit(void)
{
    printk(KERN_INFO "performancetest exit\n");
    gpio_free(PIN);
}


MODULE_LICENSE("GPL");

module_init(performancetest_init);
module_exit(performancetest_exit);


//Messung MiniDSO
//CH_A -> 1V
//TimeBase -> 20ms
//Trigger single mode, rising edge
//
//(Mit 10ms sleep)
//delta zwischen zwei 1 -> 60ms
//delta zwischen 1 und 0 -> 30,4ms
//
//(Mit 5ms sleep, TimeBase 50ms)
//-> auch um die 30ms
//
//(Ohne sleep -> kein Output auf dem MiniDSO)
//
//(mit 2ms sleep, TimeBase 20ms)
// -> 30,4ms (zw 1 und 0)
//
//
// Einflüsse auf die Messung
// - CPU
// - Scheduler
// ...
