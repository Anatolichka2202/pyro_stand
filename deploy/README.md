# Установка pyro_stand как системного сервиса (Linux)

## Требования

- Linux с systemd
- Qt6 (Core, Network, SerialPort)
- Пользователь `pyro` с доступом к `/dev/ttyUSB0`

## Сборка headless-сервиса

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SERVICE=ON
cmake --build . --target pyro_service
```

## Установка

```bash
# Копируем бинарник
sudo cp build/pyro_service /usr/local/bin/

# Копируем циклограмму
sudo mkdir -p /etc/pyro_stand
sudo cp cyclogram.ini /etc/pyro_stand/

# Создаём пользователя сервиса (если не существует)
sudo useradd -r -s /sbin/nologin pyro

# Даём доступ к COM-порту
sudo usermod -aG dialout pyro

# Устанавливаем unit-файл
sudo cp deploy/pyro_stand.service /etc/systemd/system/
sudo systemctl daemon-reload

# Включаем автозапуск и стартуем
sudo systemctl enable pyro_stand
sudo systemctl start pyro_stand
```

## Управление

```bash
sudo systemctl status pyro_stand   # статус
sudo systemctl stop pyro_stand     # остановить
sudo journalctl -u pyro_stand -f   # логи в реальном времени
```

## Обновление циклограммы

```bash
sudo cp new_cyclogram.ini /etc/pyro_stand/cyclogram.ini
sudo systemctl restart pyro_stand
```
