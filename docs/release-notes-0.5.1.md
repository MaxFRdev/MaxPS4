# MaxPS4 0.5.1 — changer de jeu sans attendre

**Cette version corrige l'attente entre deux jeux.** La correction annoncée dans la 0.5.0 ne fonctionnait pas : quitter un jeu pouvait encore bloquer le suivant de longues secondes.

C'est toujours une **préversion expérimentale**.

💬 Retours, questions et tests : **[Discord MaxPS4](https://discord.gg/G7BaXN4fd)**

## Ce qui change

- **Arrêt d'un jeu :** les threads du jeu se terminent maintenant à leur prochain appel, thread principal compris. Vérifié sur iPhone avec Mortal Kombat: Legacy Kollection et Sonic Mania.
- **Sonic Mania :** deux corrections GPU, déjà présentes dans la 0.5.0.
- **Écran de chargement :** si l'arrêt du jeu précédent prend du temps, l'app indique depuis combien de temps et où il en est.

## Installer

1. Télécharge **MaxPS4-FEXCore-SideStore.ipa** ci-dessous et installe-la avec **SideStore**.
2. Ouvre MaxPS4, touche **JIT** et active le JIT avec **StikDebug** ([protocole détaillé](https://github.com/MaxFRdev/MaxPS4/blob/main/docs/fexcore-stikdebug-jit.md)). Le JIT est indispensable.
3. Copie le **dossier complet** de ton jeu (`eboot.bin`, `sce_module`, `sce_sys` et ses données) dans *Sur mon iPhone › MaxPS4 › Jeux*, puis touche **Jouer**.

## Limites

Les mêmes que la [0.5.0](https://github.com/MaxFRdev/MaxPS4/releases/tag/v0.5.0) : seuls Sonic Mania et Mortal Kombat: Legacy Kollection sont confirmés jouables, les jeux en 3D ne s'affichent pas correctement, et aucun jeu n'est fourni.
