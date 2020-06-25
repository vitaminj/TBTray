msbuild /p:Configuration=Release /p:Platform=x64 TBTray.sln
echo TBTray register > x64\register.cmd
echo TBTray unregister > x64\unregister.cmd
