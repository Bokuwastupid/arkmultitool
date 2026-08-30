FROM mcr.microsoft.com/dotnet/sdk:10.0 AS build
WORKDIR /src
COPY Kopt.ControlPlane.csproj .
RUN dotnet restore Kopt.ControlPlane.csproj
COPY . .
RUN dotnet publish Kopt.ControlPlane.csproj -c Release --no-restore -o /app /p:UseAppHost=false

FROM mcr.microsoft.com/dotnet/aspnet:10.0 AS runtime
WORKDIR /app
COPY --from=build /app .
RUN mkdir -p /app/data-protection && chown -R app:app /app
USER app
EXPOSE 8080
ENTRYPOINT ["dotnet", "Kopt.ControlPlane.dll"]
