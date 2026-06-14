PAKET HOTSPOT OFFLINE - WIFI KOIN

Isi file:
- login.html  : halaman utama, insert coin, login voucher user=pass, login member user+pass
- status.html : halaman status user aktif
- logout.html : halaman setelah logout
- alogin.html : redirect setelah login berhasil
- error.html  : halaman error
- rlogin.html : redirect login
- md5.js      : MD5 lokal/offline untuk CHAP Mikrotik

Cara pasang:
1. Buka WinBox.
2. Masuk menu Files.
3. Buka folder hotspot.
4. Upload semua file ini ke folder hotspot, replace file lama.
5. Pastikan IP ESP32 di login.html benar:
   const ESP32_IP = "125.15.15.113";

Setting penting Mikrotik:
- ESP32 harus dibypass di IP Binding.
- IP ESP32 harus masuk Walled Garden IP supaya client belum login bisa akses ESP32.
- Voucher dari ESP32 dibuat username=password, maka tombol voucher memakai user=pass.
- Member login memakai username dan password sesuai user hotspot Mikrotik.

Terminal contoh:
 /ip hotspot walled-garden ip add action=accept dst-address=125.15.15.113 protocol=tcp dst-port=80 comment="Allow ESP32 WiFi Coin"
 /ip hotspot ip-binding add address=125.15.15.113 type=bypassed comment="ESP32_WIFI_COIN"
