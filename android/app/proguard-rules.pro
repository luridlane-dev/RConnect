# WorkManager creates its Room database by reflection (WorkDatabase_Impl's no-argument constructor).
-keep class * extends androidx.room.RoomDatabase { <init>(); }
