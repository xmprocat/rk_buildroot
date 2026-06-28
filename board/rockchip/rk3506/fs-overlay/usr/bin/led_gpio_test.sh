#!/bin/bash

# --- ..........................................GPIO...... ---
# ...................................................GPIO......
# .......................................IO 42 ............ GPIO 10 ... gpiochip32 ..................... 32 + 10 = 42...
gpio_numbers=(42 43 2 49 0 41 53 54 48 55 1 17 44 16 13 14 15 51 50 45 7 47 6 58 59 57 56 52)

# 每个脉冲的持续时间（秒）
pulse_duration=0.5

# 导出所有GPIO
for gpio in "${gpio_numbers[@]}"; do
    # 检查是否已导出，避免重复导出错误
    if [ ! -d "/sys/class/gpio/gpio$gpio" ]; then
        echo "$gpio" > /sys/class/gpio/export
        # 给内核一点时间创建目录
        sleep 0.1
    fi
    # 设置为输出模式
    echo "out" > /sys/class/gpio/gpio$gpio/direction
done

# 主循环：依次点亮并熄灭每个GPIO
while true; do
    for gpio in "${gpio_numbers[@]}"; do
        echo 0 > /sys/class/gpio/gpio$gpio/value
        sleep $pulse_duration
        echo 1 > /sys/class/gpio/gpio$gpio/value
    done
done

# 清理：取消导出所有GPIO（通常不会执行到这里）
for gpio in "${gpio_numbers[@]}"; do
    echo "$gpio" > /sys/class/gpio/unexport
done

echo "All GPIO pulses completed."

