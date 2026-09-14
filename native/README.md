# RGBCcontrol Native

Version Windows compilée de RGBCcontrol.

- Interface principale et moteur OpenRGB en C++20/Win32/GDI+.
- Détection automatique et contrôle séparé des appareils RGB.
- Pilote HID isolé pour la barre lumineuse et les cinq témoins des Sony DualSense et DualSense Edge, en USB et Bluetooth.
- Page Manette avec boutons, sticks et gâchettes affichés en direct via l'entrée brute Windows en lecture seule.
- Centre Appareils unifié avec fournisseurs, capacités, zones, LED, capteurs et état de contrôle.
- Surveillance Windows des branchements et retraits avec nouvelle analyse immédiate et temporisée.
- Effets logiciels animés envoyés par le protocole SDK OpenRGB.
- Inventaire multi-constructeur des ventilateurs via un pont isolé vers LibreHardwareMonitor.
- Profils complets persistants avec import et export `.rgbcprofile`.
- Courbe de ventilation graphique, progressive et limitée par des seuils sûrs.
- Diagnostic détaillé de la compatibilité RGB et des canaux de ventilation.
- Icône de zone de notification, démarrage Windows optionnel et planification jour/nuit.
- Centre de personnalisation : huit accents, détection réglable, trois niveaux de fluidité et options d’accessibilité.
- API de capacités versionnée servant de base sûre aux futurs adaptateurs matériels.
- Comportement Windows configurable : démarrage discret, réduction ou fermeture dans la zone de notification.
- Installateur et désinstallateur Windows NSIS.

## Construction

Depuis PowerShell :

```powershell
.\native\build.ps1
```

Les livrables sont créés dans `outputs\RGBCcontrol-CPP`.
