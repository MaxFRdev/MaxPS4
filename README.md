# MaxPS4 — émulateur PS4 expérimental pour iPhone

[![Plateforme](https://img.shields.io/badge/Plateforme-iOS%20ARM64-222222)](https://github.com/MaxFRdev/MaxPS4)
[![Statut](https://img.shields.io/badge/Statut-exp%C3%A9rimental-orange)](https://github.com/MaxFRdev/MaxPS4/actions)
[![Licence](https://img.shields.io/badge/Licence-GPL--2.0-blue)](LICENSE)
[![Jeux jouables](https://img.shields.io/badge/Jeux%20jouables-Sonic%20Mania%20%C2%B7%20Mortal%20Kombat-brightgreen)](#ce-qui-marche-aujourdhui)

<p align="center">
  <a href="https://discord.gg/99KcfSVxT8">
    <img src="https://img.shields.io/badge/Discord-Rejoindre%20la%20communaut%C3%A9%20MaxPS4-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Rejoindre le Discord MaxPS4">
  </a>
</p>

<p align="center"><b>💬 Suis l'avancement, pose tes questions et partage tes tests sur le <a href="https://discord.gg/99KcfSVxT8">Discord MaxPS4</a>.</b></p>

**MaxPS4** est un projet indépendant d'émulation PS4 sur iPhone. Le code x86-64 des jeux tourne sur l'iPhone grâce au JIT de **FEXCore** (activé avec StikDebug), les fonctions système de la PS4 sont réécrites dans MaxPS4, et le GPU de la PS4 est traduit vers **Metal**. Le projet s'appuie sur des travaux d'**AetherPS4**, de **FEXCore** et de **shadPS4**.

## Ce qui marche aujourd'hui

**Deux jeux tournent sur iPhone 15 Pro Max**, testés par l'auteur avec ses propres dossiers de jeu :

- **Sonic Mania** : près de 60 images par seconde, son, manette et sauvegardes.
- **Mortal Kombat: Legacy Kollection** : 60 images par seconde en combat, environ 54 quand le téléphone chauffe, avec le son.

Les autres jeux ne sont pas encore pris en charge ou pas encore testés (voir [Compatibilité](#compatibilité)).

### Émulation

| Partie | Ce qui est fait |
| --- | --- |
| Processeur | Code x86-64 du jeu exécuté par le JIT FEXCore sur iPhone (StikDebug), plusieurs threads du jeu en parallèle, runtime compilé en mode optimisé |
| Chargement | Dossier de jeu complet : `eboot.bin` (SELF en clair) et modules `sce_module/*.prx`, liés entre eux |
| Système PS4 | Fichiers, mémoire, threads, mutex, variables de condition, files d'événements, **sémaphores**, **event flags**, horloges et compteurs de temps précis |
| GPU | Commandes GNM/PM4 décodées et shaders GCN traduits en Metal ; textures natives filtrées par le GPU de l'iPhone ; test de profondeur et culling des faces (testés sur Mac, rendu 3D encore à valider sur iPhone). cibles couleur 8, 16 et 32 bits (dont 16 bits flottant). Pas encore de stencil : la plupart des jeux 3D ne s'affichent pas encore correctement |
| Affichage | Jusqu'à **60 images/s**, calé sur le rafraîchissement de l'écran de l'iPhone ; Ajuster / Remplir / Étirer ; 1080p ou 720p annoncé au jeu |
| Son | Sortie audio du jeu (AudioOut) jouée sur l'iPhone |
| Manette | Manettes MFi, Xbox, DualShock 4 et DualSense : toutes les touches, sticks et gâchettes analogiques, pavé tactile ; commandes tactiles réglables (taille, opacité, L1/L2/R1/R2), masquées dès qu'une manette est branchée |
| Sauvegardes | Vraies sauvegardes PS4 (`sceSaveData`) rangées dans **Sur mon iPhone › MaxPS4 › Sauvegardes › <identifiant du jeu>** |
| Économie | Un jeu qui attend l'écran dort au lieu de tourner en boucle |

### Application

- **Bibliothèque :** les jeux copiés dans **Sur mon iPhone › MaxPS4 › Jeux** avec l'app Fichiers apparaissent tout seuls, avec leur titre et leur icône ; un dossier choisi avec **Jouer** reste aussi dans la bibliothèque.
- **Accueil épuré :** liste des jeux avec un bouton Jouer, **Reprendre** le dernier jeu lancé.
- **Fiche de jeu :** image, identifiant, version, place prise par les sauvegardes et accès direct à leur dossier.
- **Pause :** ouvrir le menu du jeu met le jeu en pause.
- **Changer de jeu :** quitter un jeu et en relancer un autre sans longue attente.
- **Réglages par jeu :** affichage, résolution et commandes mémorisés pour chaque jeu.
- **6 langues :** français, anglais, espagnol, allemand, italien, portugais (Brésil), selon la langue de l'iPhone ou au choix dans les Réglages.
- **Rapport de compatibilité :** quand un jeu s'arrête, l'app dit pourquoi (fonctions PS4 manquantes, erreur) et enregistre un rapport dans **MaxPS4 › Rapports**, à partager.
- **Mode développeur** (Réglages) : options de test et rapports techniques, masqués par défaut.

## Compatibilité

| Jeu | État |
| --- | --- |
| Sonic Mania | ✅ Jouable (testé par l'auteur sur iPhone) |
| Mortal Kombat: Legacy Kollection | ✅ Jouable (testé par l'auteur sur iPhone) |
| Crash Bandicoot 4 | ❌ Bloqué : iOS refuse les adresses mémoire fixes dont les jeux Unreal Engine 4 ont besoin |
| Autres jeux 2D légers et homebrews | ❓ Non testés : à essayer, avec le rapport de compatibilité |
| Gros jeux 3D (God of War, Assassin's Creed…) | ❌ Hors de portée pour l'instant (processeur, GPU et mémoire) |

Un jeu qui bloque laisse un rapport dans **MaxPS4 › Rapports** : partage-le sur le [Discord](https://discord.gg/99KcfSVxT8) ou dans une [Issue](https://github.com/MaxFRdev/MaxPS4/issues) pour aider à ajouter ce qui manque.

## Installer et jouer

1. Télécharger l'IPA produite par le workflow [MaxPS4 FEXCore SideStore IPA](https://github.com/MaxFRdev/MaxPS4/actions/workflows/fexcore-app.yml) (artefact **MaxPS4-FEXCore-SideStore.ipa**, téléchargé directement en .ipa, sans zip) et l'installer avec **SideStore**.
2. Dans StikDebug, importer le **même PairingFile** que celui utilisé par SideStore, puis configurer la connexion et le VPN selon le guide StikDebug. Le PairingFile autorise l’appairage; il n’active pas le JIT à lui seul.
3. Revenir dans MaxPS4 et toucher **JIT** pour lancer l’activation StikDebug. Voir [le protocole SideStore + StikDebug](docs/fexcore-stikdebug-jit.md).
4. Copier le dossier complet de ton jeu (`eboot.bin`, `sce_module`, `sce_sys` et ses données) dans **Sur mon iPhone › MaxPS4 › Jeux**, puis toucher **Jouer**.

MaxPS4 ne déchiffre rien : il faut un dossier de jeu déjà extrait, issu de tes propres jeux. Le développement actif se fait sur la branche `maxps4-metal-native-sampling`.

## Conformité et responsabilité

- **Licences** : MaxPS4 indique la [GNU GPL v2.0](LICENSE). Chaque source ou dépendance tierce doit être attribuée, auditée et redistribuée dans le respect de sa propre licence. Aucun audit de conformité complet n'est revendiqué.
- **Attribution** : contributions et propriété intellectuelle des auteurs originaux conservées. [shadPS4](https://github.com/shadps4-emu/shadPS4), [FEX](https://github.com/FEX-Emu/FEX) et [AetherPS4](https://github.com/Leviidev/AetherPS4) sont des projets distincts.
- **Marques** : MaxPS4 est indépendant et **n'est affilié ni à Sony Interactive Entertainment / PlayStation ni aux équipes officielles des projets cités**.
- **Contenus** : aucun jeu commercial, firmware PS4, clé, contenu chiffré protégé ou fichier PKG n'est fourni, et MaxPS4 ne contourne aucune protection. N'utilise que des jeux que tu possèdes.
- **App Store** : la publication sur l'App Store n'est pas un objectif ; la distribution expérimentale par IPA reste soumise aux licences et aux restrictions de signature iOS.

Voir le document détaillé [Conformité, attribution et limites légales](docs/LEGAL-COMPLIANCE.md).

## Contribuer et communauté

Les contributions sont bienvenues via [Issues](https://github.com/MaxFRdev/MaxPS4/issues), [Pull Requests](https://github.com/MaxFRdev/MaxPS4/pulls) et [CONTRIBUTING.md](CONTRIBUTING.md). Les [GitHub Actions](https://github.com/MaxFRdev/MaxPS4/actions) compilent l'IPA et lancent les tests ; les builds de développement ne sont pas des versions stables.

[![Discord](https://img.shields.io/badge/Discord-Rejoindre%20MaxPS4-5865F2?logo=discord&logoColor=white)](https://discord.gg/99KcfSVxT8)

**[Rejoindre le Discord MaxPS4](https://discord.gg/99KcfSVxT8)**

## Documentation technique

- [Traduction du GPU PS4 vers Metal (Sonic Mania)](docs/sonic-gpu.md)
- [VideoOut et affichage](docs/videoout-linear.md)
- [Services système PS4](docs/ps4-system-services.md)
- [Fondation runtime Orbis](docs/orbis-runtime-foundation.md)
- [Analyse ELF Orbis/SCE et PRX](docs/orbis-sce-prx-analysis.md)
- [IPA FEXCore pour SideStore](docs/fexcore-app-sidestore.md)

---
*Dernière mise à jour : 5 novembre 2026.*
