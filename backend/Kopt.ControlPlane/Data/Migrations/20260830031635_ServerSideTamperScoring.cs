using Microsoft.EntityFrameworkCore.Migrations;

#nullable disable

namespace Kopt.ControlPlane.Data.Migrations
{
    /// <inheritdoc />
    public partial class ServerSideTamperScoring : Migration
    {
        /// <inheritdoc />
        protected override void Up(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.AddColumn<string>(
                name: "SignalsJson",
                table: "TamperIncidents",
                type: "text",
                nullable: false,
                defaultValue: "");
        }

        /// <inheritdoc />
        protected override void Down(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.DropColumn(
                name: "SignalsJson",
                table: "TamperIncidents");
        }
    }
}
