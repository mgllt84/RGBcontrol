# RGBCcontrol Plugin SDK v2

Le moteur v2 ajoute des pilotes matériels sans reconstruire l'application.
Un pilote est un exécutable autonome accompagné d'un manifeste
`.rgbcplugin`. Il est lancé hors du processus de l'interface : un plantage du
pilote ne peut donc pas faire planter RGBCcontrol.

## Sécurité

- RGBCcontrol refuse un manifeste dont l'API, le VID/PID ou le chemin est
  invalide.
- l'exécutable doit rester sous le dossier `Plugins` ; les chemins absolus et
  les sorties de dossier sont refusés ;
- son SHA-256 doit être exactement celui inscrit dans le manifeste ;
- le pilote doit répondre en moins de `TimeoutMs` ; sinon seul son processus
  est arrêté ;
- aucun appareil n'est ajouté comme contrôlable tant que la commande `probe`
  n'a pas réussi.

Une empreinte protège l'intégrité mais ne remplace pas une signature d'éditeur.
Les packages distribués au public devront être audités puis signés par le
projet RGBCcontrol.

## Manifeste

```ini
[RGBCPlugin]
Api=2
Enabled=1
Id=com.fabricant.modele
Name=Pilote Modèle RGB
Publisher=Nom de l'éditeur
Transport=hid
Executable=modele-driver.exe
Sha256=64_caracteres_hexadecimaux
Vid=0x1234
Pid=0x5678
DeviceName=Modèle RGB
DeviceType=Clavier
Capabilities=lighting,effects,zones,perled
Modes=Static,Breathing,Rainbow,Wave,Gradient
Zones=1
Leds=104
TimeoutMs=5000
SupersedesOpenRGB=Nom éventuellement affiché par OpenRGB
```

Le couple `Vid`/`Pid` est une correspondance exacte. Un pilote prenant en
charge plusieurs couples possède un manifeste par couple ; ils peuvent tous
référencer le même exécutable.

Capacités reconnues : `lighting`, `effects`, `zones`, `perled`, `sensors`,
`fanread`, `fancontrol`, `hotplug`. Le moteur ajoute toujours `discovery` et
`localonly`.

## Protocole de processus

RGBCcontrol lance le pilote avec les arguments suivants :

```text
--rgbc-api 2 --command probe  --instance "ID Windows"
--rgbc-api 2 --command static --instance "ID Windows" --color RRGGBB --brightness 0..100
--rgbc-api 2 --command effect --instance "ID Windows" --mode "Rainbow" --color RRGGBB --brightness 0..100 --speed 0..100 --intensity 0..100
--rgbc-api 2 --command stop   --instance "ID Windows"
```

Une réussite utilise le code de sortie `0` et commence impérativement stdout
par :

```text
RGBCPLUGIN/2<TAB>OK
```

La réponse à `probe` peut ensuite préciser :

```text
NAME=Nom exact détecté
VENDOR=Fabricant
TYPE=Clavier
ZONES=1
LEDS=104
```

Toute autre sortie ou tout autre code est un refus. Le texte est repris dans
le rapport de diagnostic afin d'éviter les faux « compatible ».

## Règles d'un pilote matériel

Le pilote doit ouvrir uniquement l'instance transmise, borner toutes les
tailles de paquets et restaurer un état matériel sûr avec `stop`. Les écritures
de firmware sont interdites. Un protocole provenant d'un projet tiers ne peut
être adapté que si sa licence autorise clairement cette réutilisation et si
son avis de licence accompagne le pilote.

Cette première révision couvre les couleurs statiques et les effets matériels
gérés par le pilote. Le streaming de trames par LED sera ajouté à une révision
ultérieure du protocole sans casser les plugins v2.
