# Push cod la GitHub – Alarma_1_simpla

Repo: **https://github.com/tzonecata/Alarma_1_simpla.git**

## Pași (rulează în PowerShell sau CMD, în acest folder)

### 1. Șterge .git stricat (dacă există)
```powershell
cd "c:\__tz_pers\CCS\Alarma simpla"
Remove-Item -Recurse -Force .git -ErrorAction SilentlyContinue
```

### 2. Inițializare, add, commit
```powershell
git init
git add .
git commit -m "Alarma ESP8266: 4 PIR, 2 relee, WiFi, MQTT pe alarma_simpla"
```

### 3. Conectare la GitHub și push
```powershell
git remote add origin https://github.com/tzonecata/Alarma_1_simpla.git
git branch -M main
git push -u origin main
```

Dacă ceri parolă/token: folosește un **Personal Access Token** (GitHub → Settings → Developer settings → Personal access tokens), nu parola contului.

### Dacă „remote origin already exists”
```powershell
git remote set-url origin https://github.com/tzonecata/Alarma_1_simpla.git
git push -u origin main
```
