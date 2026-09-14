# RGBCcontrol Device Provider API v2

RGBCcontrol décrit chaque moteur matériel comme un fournisseur et chaque
opération comme une capacité explicite définie dans `src/device_provider.hpp`.
Cette séparation permet d’ajouter un constructeur sans donner à son adaptateur
plus de droits que nécessaire.

## Fournisseurs intégrés

- `openrgb` : découverte, éclairage, effets, zones et LED individuelles via le
  SDK TCP local d’OpenRGB.
- `hardware` : capteurs, lecture et contrôle de ventilation via le pont isolé
  LibreHardwareMonitor.
- `hotplug` : arrivée et retrait des périphériques signalés par Windows avec
  `WM_DEVICECHANGE`.

## Contrat d’un nouveau fournisseur

Un adaptateur doit fournir un identifiant stable, un nom, son transport et
uniquement les drapeaux `DeviceCapability` réellement disponibles. Les
identifiants d’appareil doivent rester stables entre deux analyses afin de
conserver les profils.

Toute écriture matérielle doit être déclenchée par une action explicite, être
bornée par les limites du matériel et posséder une méthode de retour au mode
automatique. Les écritures de firmware et les protocoles HID expérimentaux ne
font pas partie de l’API sûre.

## Ajout d’un adaptateur interne

1. Ajouter son descriptif au catalogue des fournisseurs.
2. Exécuter sa découverte dans le thread d’analyse sans bloquer l’interface.
3. Traduire ses fonctions en capacités communes.
4. Afficher les fonctions indisponibles en lecture seule dans le Centre
   Appareils.
5. Ajouter un test de découverte, un test de retour au mode sûr et une capture
   d’interface avant publication.

## Plugins matériels externes

L'API v2 charge des adaptateurs sous forme de processus isolés, jamais comme
DLL dans l'interface. Un manifeste fait correspondre un VID/PID exact à un
exécutable dont l'empreinte SHA-256 est vérifiée. La commande de découverte
doit réussir avant que le périphérique soit déclaré pilotable.

Le contrat détaillé, le manifeste et les commandes sont décrits dans
`PLUGIN-SDK.md`. Cette base permet d'ajouter un constructeur sans recompiler
RGBCcontrol tout en conservant OpenRGB comme fournisseur de repli.
